#include "learning.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace rizoma::learning {

Strategy parse_strategy(const std::string& name) {
  if (name == "BAYESIAN_CALIBRATED" || name == "bayesian") return Strategy::BAYESIAN_CALIBRATED;
  if (name == "TABULAR_INCREMENTAL" || name == "tabular") return Strategy::TABULAR_INCREMENTAL;
  if (name == "VISUAL_KNN" || name == "knn" || name == "visual") return Strategy::VISUAL_KNN;
  return Strategy::BASELINE_HEURISTIC;
}

std::string strategy_name(Strategy strategy) {
  switch (strategy) {
    case Strategy::BAYESIAN_CALIBRATED: return "BAYESIAN_CALIBRATED";
    case Strategy::TABULAR_INCREMENTAL: return "TABULAR_INCREMENTAL";
    case Strategy::VISUAL_KNN: return "VISUAL_KNN";
    default: return "BASELINE_HEURISTIC";
  }
}

FeatureVector extract_features(const analysis::Region& region, std::uint64_t image_width, std::uint64_t image_height) {
  FeatureVector f;
  const double a = std::max(1.0, static_cast<double>(region.area));
  f.values[0] = static_cast<double>(region.local_pixels) / a;
  f.values[1] = static_cast<double>(region.global_pixels) / a;
  f.values[2] = static_cast<double>(region.shared_pixels) / a;
  const double total_img_area = std::max(1.0, static_cast<double>(image_width * image_height));
  f.values[3] = std::log1p(a) / std::log1p(total_img_area);
  const double w = std::max(1.0, static_cast<double>(region.width));
  const double h = std::max(1.0, static_cast<double>(region.height));
  f.values[4] = std::min(10.0, std::max(w / h, h / w)) / 10.0;
  f.values[5] = region.touches_border ? 1.0 : 0.0;
  return f;
}

JudgmentSuggestion infer_bayesian(sqlite3* db, const std::string& user_id, const analysis::Region& region, double global_mean, double global_stddev) {
  (void)global_mean; (void)global_stddev;
  const bool has_shared = region.shared_pixels > 0;
  
  // Calculate empirical Bayes probabilities from historical review_events
  sqlite3_stmt* stmt = nullptr;
  const char* query = R"SQL(
    SELECT e.label, COUNT(*) FROM review_events e
    JOIN analysis_regions r ON r.id = e.region_id
    JOIN analysis_runs a ON a.id = r.run_id
    JOIN images i ON i.id = a.image_id
    WHERE (i.user_id = ? OR ? = '') AND e.action IN ('CONFIRM','CORRECT')
      AND ((? = 1 AND r.area > 0 AND (CAST(json_extract(r.evidence_json, '$.shared_pixels') AS INT) > 0))
        OR (? = 0 AND (CAST(json_extract(r.evidence_json, '$.shared_pixels') AS INT) = 0)))
    GROUP BY e.label
  )SQL";

  int root_count = 0, other_count = 0;
  if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, has_shared ? 1 : 0);
    sqlite3_bind_int(stmt, 4, has_shared ? 1 : 0);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* label_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      const int cnt = sqlite3_column_int(stmt, 1);
      if (label_ptr && std::string(label_ptr) == "ROOT") {
        root_count += cnt;
      } else {
        other_count += cnt;
      }
    }
    sqlite3_finalize(stmt);
  }

  // Laplace-smoothed empirical probability
  const double prior_alpha = has_shared ? 3.0 : 1.0;
  const double prior_beta = has_shared ? 1.0 : 3.0;
  const double p_root = (static_cast<double>(root_count) + prior_alpha) /
                        (static_cast<double>(root_count + other_count) + prior_alpha + prior_beta);

  JudgmentSuggestion s;
  s.label = p_root >= 0.50 ? "ROOT" : "UNKNOWN";
  s.confidence = p_root >= 0.50 ? p_root : (1.0 - p_root);

  std::ostringstream details;
  details << "{\"strategy\":\"BAYESIAN_CALIBRATED\",\"calibrated_p_root\":" << std::fixed << std::setprecision(4)
          << p_root << ",\"samples_in_bucket\":" << (root_count + other_count)
          << ",\"has_shared_pixels\":" << (has_shared ? "true" : "false") << "}";
  s.details_json = details.str();
  return s;
}

static std::pair<std::vector<double>, double> load_tabular_weights(sqlite3* db, const std::string& user_id, int& samples) {
  std::vector<double> w = {0.8, 0.4, 1.2, 0.1, -0.2, -0.5}; // baseline heuristic seed weights
  double bias = -0.3;
  samples = 0;

  sqlite3_stmt* stmt = nullptr;
  const char* q = "SELECT weights_json, sample_count FROM learning_models WHERE user_id=? AND model_type='TABULAR_LOGISTIC'";
  if (sqlite3_prepare_v2(db, q, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* json_txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      samples = sqlite3_column_int(stmt, 1);
      if (json_txt) {
        // Minimal parse of 6 weights and 1 bias
        std::string s(json_txt);
        auto pos_b = s.find("\"bias\":");
        if (pos_b != std::string::npos) {
          bias = std::stod(s.substr(pos_b + 7));
        }
        auto pos_w = s.find("\"weights\":[");
        if (pos_w != std::string::npos) {
          std::stringstream ss(s.substr(pos_w + 11));
          for (int i = 0; i < 6; ++i) {
            double val = 0; char sep = 0;
            if (ss >> val) { w[i] = val; ss >> sep; }
          }
        }
      }
    }
    sqlite3_finalize(stmt);
  }
  return {w, bias};
}

JudgmentSuggestion infer_tabular(sqlite3* db, const std::string& user_id, const FeatureVector& features) {
  int samples = 0;
  auto [w, b] = load_tabular_weights(db, user_id, samples);

  double dot = b;
  for (std::size_t i = 0; i < features.values.size() && i < w.size(); ++i) {
    dot += w[i] * features.values[i];
  }
  const double prob = 1.0 / (1.0 + std::exp(-std::clamp(dot, -15.0, 15.0)));

  JudgmentSuggestion s;
  s.label = prob >= 0.50 ? "ROOT" : "UNKNOWN";
  s.confidence = prob >= 0.50 ? prob : (1.0 - prob);

  std::ostringstream details;
  details << "{\"strategy\":\"TABULAR_INCREMENTAL\",\"predicted_prob\":" << std::fixed << std::setprecision(4)
          << prob << ",\"model_samples\":" << samples << ",\"z_score\":" << dot << "}";
  s.details_json = details.str();
  return s;
}

void train_tabular_step(sqlite3* db, const std::string& user_id, const FeatureVector& features, bool is_root) {
  int samples = 0;
  auto [w, b] = load_tabular_weights(db, user_id, samples);

  double dot = b;
  for (std::size_t i = 0; i < features.values.size() && i < w.size(); ++i) {
    dot += w[i] * features.values[i];
  }
  const double prob = 1.0 / (1.0 + std::exp(-std::clamp(dot, -15.0, 15.0)));
  const double target = is_root ? 1.0 : 0.0;
  const double error = target - prob;

  // Learning rate with annealing
  const double lr = 0.15 / std::sqrt(1.0 + static_cast<double>(samples) * 0.05);

  b = std::clamp(b + lr * error, -10.0, 10.0);
  for (std::size_t i = 0; i < features.values.size() && i < w.size(); ++i) {
    w[i] = std::clamp(w[i] + lr * error * features.values[i], -10.0, 10.0);
  }
  ++samples;

  std::ostringstream json;
  json << "{\"bias\":" << std::fixed << std::setprecision(5) << b << ",\"weights\":[";
  for (std::size_t i = 0; i < w.size(); ++i) {
    if (i > 0) json << ",";
    json << w[i];
  }
  json << "]}";
  const std::string json_str = json.str();

  sqlite3_stmt* stmt = nullptr;
  const char* upsert = R"SQL(
    INSERT INTO learning_models(id, user_id, model_type, weights_json, sample_count, updated_at)
    VALUES(?, ?, 'TABULAR_LOGISTIC', ?, ?, datetime('now'))
    ON CONFLICT(id) DO UPDATE SET
      weights_json=excluded.weights_json,
      sample_count=excluded.sample_count,
      updated_at=excluded.updated_at
  )SQL";

  if (sqlite3_prepare_v2(db, upsert, -1, &stmt, nullptr) == SQLITE_OK) {
    const std::string model_id = user_id + "_tabular";
    sqlite3_bind_text(stmt, 1, model_id.data(), static_cast<int>(model_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, json_str.data(), static_cast<int>(json_str.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, samples);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

VisualDescriptor extract_descriptor(const std::vector<unsigned char>& rgba, std::uint64_t width, std::uint64_t height, const analysis::Region& region) {
  VisualDescriptor d;
  if (rgba.size() != width * height * 4) return d;

  double sum = 0, sq = 0;
  std::uint64_t total_px = 0;
  const auto x1 = std::min(width, region.x + region.width);
  const auto y1 = std::min(height, region.y + region.height);

  for (auto y = region.y; y < y1; ++y) {
    for (auto x = region.x; x < x1; ++x) {
      const auto idx = static_cast<std::size_t>((y * width + x) * 4);
      const auto r = rgba[idx], g = rgba[idx + 1], b = rgba[idx + 2];
      const auto gray = static_cast<unsigned char>(std::clamp(std::lround(0.299 * r + 0.587 * g + 0.114 * b), 0L, 255L));
      const int bin = std::clamp(static_cast<int>(gray / 32), 0, 7);
      d.histogram[bin] += 1.0;
      sum += gray;
      sq += double(gray) * gray;
      ++total_px;
    }
  }

  if (total_px > 0) {
    for (auto& bin : d.histogram) bin /= static_cast<double>(total_px);
    const double mean = sum / total_px;
    d.variance = std::max(0.0, sq / total_px - mean * mean) / (255.0 * 255.0);
  }
  return d;
}

JudgmentSuggestion infer_visual_knn(sqlite3* db, const std::string& user_id, const VisualDescriptor& descriptor) {
  sqlite3_stmt* stmt = nullptr;
  const char* q = "SELECT label, descriptor_json FROM learning_exemplars WHERE user_id=? OR ?='' LIMIT 100";
  
  std::vector<std::pair<double, std::string>> neighbors; // <distance, label>
  if (sqlite3_prepare_v2(db, q, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const auto* label_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      const auto* json_ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      if (!label_ptr || !json_ptr) continue;
      const std::string label = label_ptr;
      std::string s = json_ptr;

      VisualDescriptor other;
      auto pos_v = s.find("\"variance\":");
      if (pos_v != std::string::npos) other.variance = std::stod(s.substr(pos_v + 11));
      auto pos_h = s.find("\"hist\":[");
      if (pos_h != std::string::npos) {
        std::stringstream ss(s.substr(pos_h + 8));
        for (int i = 0; i < 8; ++i) {
          double val = 0; char sep = 0;
          if (ss >> val) { other.histogram[i] = val; ss >> sep; }
        }
      }

      // Euclidean distance in descriptor space
      double dist_sq = 0;
      for (int i = 0; i < 8; ++i) {
        const double diff = descriptor.histogram[i] - other.histogram[i];
        dist_sq += diff * diff;
      }
      const double v_diff = descriptor.variance - other.variance;
      dist_sq += 2.0 * v_diff * v_diff;
      neighbors.emplace_back(std::sqrt(dist_sq), label);
    }
    sqlite3_finalize(stmt);
  }

  JudgmentSuggestion s;
  if (neighbors.empty()) {
    // Cold start fallback
    s.label = "UNKNOWN";
    s.confidence = 0.5;
    s.details_json = "{\"strategy\":\"VISUAL_KNN\",\"cold_start\":true,\"confidence\":0.5}";
    return s;
  }

  std::sort(neighbors.begin(), neighbors.end());
  const std::size_t k = std::min<std::size_t>(5, neighbors.size());
  int root_votes = 0;
  double min_dist = neighbors.front().first;
  for (std::size_t i = 0; i < k; ++i) {
    if (neighbors[i].second == "ROOT") ++root_votes;
  }

  const double vote_ratio = static_cast<double>(root_votes) / static_cast<double>(k);
  const bool uncertain = (min_dist > 0.45) || (vote_ratio > 0.35 && vote_ratio < 0.65);
  s.label = vote_ratio >= 0.50 ? "ROOT" : "UNKNOWN";
  s.confidence = vote_ratio >= 0.50 ? vote_ratio : (1.0 - vote_ratio);

  std::ostringstream details;
  details << "{\"strategy\":\"VISUAL_KNN\",\"k_neighbors\":" << k
          << ",\"root_votes\":" << root_votes
          << ",\"min_distance\":" << std::fixed << std::setprecision(4) << min_dist
          << ",\"uncertainty\":" << (uncertain ? "true" : "false") << "}";
  s.details_json = details.str();
  return s;
}

void record_visual_exemplar(sqlite3* db, const std::string& user_id, const VisualDescriptor& descriptor, const std::string& label) {
  std::ostringstream json;
  json << "{\"variance\":" << std::fixed << std::setprecision(5) << descriptor.variance << ",\"hist\":[";
  for (int i = 0; i < 8; ++i) {
    if (i > 0) json << ",";
    json << descriptor.histogram[i];
  }
  json << "]}";
  const std::string desc_json = json.str();

  sqlite3_stmt* stmt = nullptr;
  const char* insert = R"SQL(
    INSERT INTO learning_exemplars(id, user_id, label, descriptor_json, created_at)
    VALUES(hex(randomblob(12)), ?, ?, ?, datetime('now'))
  )SQL";
  if (sqlite3_prepare_v2(db, insert, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, user_id.data(), static_cast<int>(user_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, label.data(), static_cast<int>(label.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, desc_json.data(), static_cast<int>(desc_json.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

RecalibrationResult recalibrate_pending_regions(
    sqlite3* db,
    const std::string& user_id,
    const std::string& run_id,
    const std::string& triggering_region_id,
    const std::string& now_utc,
    std::uint64_t image_width,
    std::uint64_t image_height,
    const std::vector<unsigned char>* rgba
) {
  RecalibrationResult res;
  if (!db || run_id.empty()) return res;

  std::string algo_name;
  {
    sqlite3_stmt* run_st = nullptr;
    const char* q = "SELECT algorithm FROM analysis_runs WHERE id=?";
    if (sqlite3_prepare_v2(db, q, -1, &run_st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(run_st, 1, run_id.data(), static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
      if (sqlite3_step(run_st) == SQLITE_ROW) {
        const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(run_st, 0));
        if (txt) algo_name = txt;
      }
      sqlite3_finalize(run_st);
    }
  }

  const auto strategy = parse_strategy(algo_name);
  if (strategy == Strategy::BASELINE_HEURISTIC) {
    return res;
  }

  const char* q_pending = R"SQL(
    SELECT r.id, r.x, r.y, r.width, r.height, r.area, r.evidence_json,
           CAST(COALESCE(json_extract(r.evidence_json, '$.global_pixels'), 0) AS INT),
           CAST(COALESCE(json_extract(r.evidence_json, '$.local_pixels'), 0) AS INT),
           CAST(COALESCE(json_extract(r.evidence_json, '$.shared_pixels'), 0) AS INT),
           CAST(COALESCE(json_extract(r.evidence_json, '$.touches_border'), 0) AS INT),
           CAST(COALESCE(json_extract(r.evidence_json, '$.global_mean'), 128.0) AS REAL),
           CAST(COALESCE(json_extract(r.evidence_json, '$.global_stddev'), 20.0) AS REAL),
           j.label
    FROM analysis_regions r
    JOIN analysis_judgments j ON j.id = (
      SELECT j2.id FROM analysis_judgments j2
      WHERE j2.region_id = r.id AND j2.actor_type = 'ALGORITHM'
      ORDER BY j2.created_at DESC, j2.rowid DESC LIMIT 1
    )
    WHERE r.run_id = ? AND r.current_version = 0 AND r.id != ?
    ORDER BY r.ordinal
  )SQL";

  struct PendingRegionData {
    std::string id;
    analysis::Region reg;
    std::string evidence_json;
    double global_mean{128.0};
    double global_stddev{20.0};
    std::string prev_label;
  };

  std::vector<PendingRegionData> pending_list;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, q_pending, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, run_id.data(), static_cast<int>(run_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, triggering_region_id.data(), static_cast<int>(triggering_region_id.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
      PendingRegionData pr;
      const auto* pid = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
      if (!pid) continue;
      pr.id = pid;
      pr.reg.x = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
      pr.reg.y = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
      pr.reg.width = static_cast<std::uint64_t>(sqlite3_column_int64(st, 3));
      pr.reg.height = static_cast<std::uint64_t>(sqlite3_column_int64(st, 4));
      pr.reg.area = static_cast<std::uint64_t>(sqlite3_column_int64(st, 5));
      const auto* ev = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
      pr.evidence_json = ev ? ev : "{}";
      pr.reg.global_pixels = static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
      pr.reg.local_pixels = static_cast<std::uint64_t>(sqlite3_column_int64(st, 8));
      pr.reg.shared_pixels = static_cast<std::uint64_t>(sqlite3_column_int64(st, 9));
      pr.reg.touches_border = sqlite3_column_int(st, 10) != 0;
      pr.global_mean = sqlite3_column_double(st, 11);
      pr.global_stddev = sqlite3_column_double(st, 12);
      const auto* pl = reinterpret_cast<const char*>(sqlite3_column_text(st, 13));
      pr.prev_label = pl ? pl : "UNKNOWN";
      pending_list.push_back(std::move(pr));
    }
    sqlite3_finalize(st);
  }

  if (pending_list.empty()) return res;

  const char* q_insert = R"SQL(
    INSERT INTO analysis_judgments(id, region_id, actor_type, label, evidence_json, created_at)
    VALUES(lower(hex(randomblob(12))), ?, 'ALGORITHM', ?, ?, ?)
  )SQL";
  sqlite3_stmt* ins_stmt = nullptr;
  if (sqlite3_prepare_v2(db, q_insert, -1, &ins_stmt, nullptr) != SQLITE_OK) {
    return res;
  }

  for (const auto& pr : pending_list) {
    JudgmentSuggestion suggestion;
    if (strategy == Strategy::BAYESIAN_CALIBRATED) {
      suggestion = infer_bayesian(db, user_id, pr.reg, pr.global_mean, pr.global_stddev);
    } else if (strategy == Strategy::TABULAR_INCREMENTAL) {
      auto feat = extract_features(pr.reg, image_width, image_height);
      suggestion = infer_tabular(db, user_id, feat);
    } else if (strategy == Strategy::VISUAL_KNN) {
      if (rgba && rgba->size() == image_width * image_height * 4) {
        auto desc = extract_descriptor(*rgba, image_width, image_height, pr.reg);
        suggestion = infer_visual_knn(db, user_id, desc);
      } else {
        suggestion.label = pr.prev_label;
        suggestion.confidence = 0.5;
        suggestion.details_json = "{\"strategy\":\"VISUAL_KNN\",\"fallback\":true}";
      }
    }

    std::ostringstream comb;
    comb << "{\"evidence\":" << pr.evidence_json
         << ",\"learning\":" << suggestion.details_json
         << ",\"confidence\":" << std::fixed << std::setprecision(4) << suggestion.confidence
         << ",\"recalibrated\":true"
         << ",\"trigger_region_id\":\"" << triggering_region_id << "\"}";
    std::string comb_str = comb.str();

    sqlite3_reset(ins_stmt);
    sqlite3_bind_text(ins_stmt, 1, pr.id.data(), static_cast<int>(pr.id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 2, suggestion.label.data(), static_cast<int>(suggestion.label.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 3, comb_str.data(), static_cast<int>(comb_str.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(ins_stmt, 4, now_utc.data(), static_cast<int>(now_utc.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(ins_stmt) == SQLITE_DONE) {
      ++res.recalibrated_count;
    }
  }
  sqlite3_finalize(ins_stmt);

  return res;
}

} // namespace rizoma::learning
