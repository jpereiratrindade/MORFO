#pragma once

#include "analysis.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace rizoma::learning {

enum class Strategy {
  BASELINE_HEURISTIC,
  BAYESIAN_CALIBRATED,
  TABULAR_INCREMENTAL,
  VISUAL_KNN
};

Strategy parse_strategy(const std::string& name);
std::string strategy_name(Strategy strategy);

struct FeatureVector {
  std::array<double, 6> values{}; // [local_ratio, global_ratio, shared_ratio, log_area, aspect_ratio, touches_border]
};

FeatureVector extract_features(const analysis::Region& region, std::uint64_t image_width, std::uint64_t image_height);

struct JudgmentSuggestion {
  std::string label;       // "ROOT" or "UNKNOWN"
  double confidence{0.5};  // 0.0 to 1.0
  std::string details_json;
};

// 1. Level 1: Bayesian / Dynamic Threshold Calibration
JudgmentSuggestion infer_bayesian(sqlite3* db, const std::string& user_id, const analysis::Region& region, double global_mean, double global_stddev);

// 2. Level 2: Tabular Online Logistic Regression (SGD)
JudgmentSuggestion infer_tabular(sqlite3* db, const std::string& user_id, const FeatureVector& features);
void train_tabular_step(sqlite3* db, const std::string& user_id, const FeatureVector& features, bool is_root);

// 3. Level 3: Visual Patch Similarity / k-NN & Uncertainty
struct VisualDescriptor {
  std::array<double, 8> histogram{}; // 8-bin normalized grayscale histogram
  double variance{};
};

VisualDescriptor extract_descriptor(const std::vector<unsigned char>& rgba, std::uint64_t width, std::uint64_t height, const analysis::Region& region);
JudgmentSuggestion infer_visual_knn(sqlite3* db, const std::string& user_id, const VisualDescriptor& descriptor);
void record_visual_exemplar(sqlite3* db, const std::string& user_id, const VisualDescriptor& descriptor, const std::string& label);

// 4. Real-time online recalibration of pending regions
struct RecalibrationResult {
  int recalibrated_count{0};
};

RecalibrationResult recalibrate_pending_regions(
    sqlite3* db,
    const std::string& user_id,
    const std::string& run_id,
    const std::string& triggering_region_id,
    const std::string& now_utc,
    std::uint64_t image_width,
    std::uint64_t image_height,
    const std::vector<unsigned char>* rgba = nullptr
);

} // namespace rizoma::learning
