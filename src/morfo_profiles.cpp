#include "morfo_profiles.hpp"
#include "rizoma.hpp" // EXP-003 original: namespace retained for provenance
#include <fstream>
#include <stdexcept>

namespace morfo::profiles {
void create_schema(sqlite3* db) {
 const char* schema=R"SQL(
 CREATE TABLE IF NOT EXISTS morfo_profile_runs (
  id TEXT PRIMARY KEY,
  image_id TEXT NOT NULL REFERENCES images(id),
  user_id TEXT NOT NULL REFERENCES users(id),
  profile TEXT NOT NULL,
  version TEXT NOT NULL,
  original_sha256 TEXT NOT NULL,
  mask_sha256 TEXT NOT NULL,
  width INTEGER NOT NULL,
  height INTEGER NOT NULL,
  candidate_pixels INTEGER NOT NULL,
  components INTEGER NOT NULL,
  border_abstentions INTEGER NOT NULL,
  relative_dir TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL
 );
 CREATE INDEX IF NOT EXISTS idx_morfo_profile_runs_image ON morfo_profile_runs(image_id,created_at);
 CREATE TABLE IF NOT EXISTS morfo_root_metric_runs (
  id TEXT PRIMARY KEY,
  profile_run_id TEXT NOT NULL REFERENCES morfo_profile_runs(id),
  image_id TEXT NOT NULL REFERENCES images(id),
  user_id TEXT NOT NULL REFERENCES users(id),
  metrics_version TEXT NOT NULL,
  mask_sha256 TEXT NOT NULL,
  calibration_json TEXT NOT NULL,
  result_json TEXT NOT NULL,
  created_at TEXT NOT NULL
 );
 CREATE INDEX IF NOT EXISTS idx_morfo_root_metric_runs_profile ON morfo_root_metric_runs(profile_run_id,created_at DESC);
 CREATE TABLE IF NOT EXISTS morfo_root_metric_components (
  metric_run_id TEXT NOT NULL REFERENCES morfo_root_metric_runs(id),
  component_id INTEGER NOT NULL,
  area_px INTEGER NOT NULL,
  area_mm2 REAL,
  centroid_x REAL NOT NULL,
  centroid_y REAL NOT NULL,
  min_x INTEGER NOT NULL,min_y INTEGER NOT NULL,max_x INTEGER NOT NULL,max_y INTEGER NOT NULL,
  touches_image_boundary INTEGER NOT NULL,
  touches_roi_boundary INTEGER NOT NULL,
  touches_abstained_area INTEGER NOT NULL,
  PRIMARY KEY(metric_run_id,component_id)
 );
 )SQL";
 char* error=nullptr;
 if(sqlite3_exec(db,schema,nullptr,nullptr,&error)!=SQLITE_OK){
  std::string message=error?error:"schema error";sqlite3_free(error);
  throw std::runtime_error("MORFO profile schema: "+message);
 }
}
std::string registry_json(){
 return R"JSON({"data":[{"id":"roots-exp003","theme":"roots","name":"Raízes · Matriz adaptativa EXP-003","version":"exp003-original-0.1.0","status":"EXPERIMENTAL","output_semantics":"ANOMALY_CANDIDATE","biological_validation":"NOT_VALIDATED"},{"id":"discovery","theme":"generic","name":"Descoberta emergente REA/RIT","status":"PLANNED","output_semantics":"UNIMPLEMENTED"}]})JSON";
}
RootsOutput run_exp003(const fs::path& source,const fs::path& out_dir){
 // Preserve EXACT detector source in profiles/roots/exp003/src/core.cpp.
 // Never overwrite an input or produce validated biological claims.
 if(!fs::is_regular_file(source))throw std::runtime_error("Original indisponível");
 if(fs::exists(out_dir))throw std::runtime_error("Execução duplicada");
 const auto img=rizoma::read_rgb(source);
 if(img.width<=0||img.height<=0||static_cast<std::uint64_t>(img.width)*img.height>12'000'000ULL)
  throw std::runtime_error("Perfil EXP-003 limitado a 12 MP; nao reduzir a imagem silenciosamente");
 const auto g=rizoma::grayscale(img);
 const rizoma::Params opts{};
 const auto th=rizoma::tophat(g,opts.top_hat_radius);
 const auto detection=rizoma::adaptive_detect(img,g,th,opts);
 const auto summary=rizoma::summarize(detection.mask);
 fs::create_directories(out_dir);
 try {
  rizoma::write_png(out_dir/"mask.png",detection.mask);
  rizoma::write_png(out_dir/"overlay.png",rizoma::overlay(img,detection.mask));
  RootsOutput result{static_cast<std::uint64_t>(img.width),static_cast<std::uint64_t>(img.height),summary.pixels,summary.components,detection.border_abstentions,rizoma::sha256_file(out_dir/"mask.png"),rizoma::sha256_file(source)};
  std::ofstream report(out_dir/"report.json",std::ios::binary);
  report<<"{\"schema\":\"morfo.profile.roots.exp003.v1\",\"profile\":\"roots-exp003\",\"version\":\"exp003-original-0.1.0\",\"scientific_status\":\"NOT_VALIDATED\",\"classification\":\"ANOMALY_CANDIDATE\",\"root_identity\":\"UNKNOWN\",\"parameters\":{\"cell\":32,\"context\":64,\"border\":10,\"seed_z\":2.4,\"top_hat_radius\":8,\"min_tophat\":7},\"original_sha256\":\""<<result.original_sha256<<"\",\"mask_sha256\":\""<<result.mask_sha256<<"\",\"width\":"<<result.width<<",\"height\":"<<result.height<<",\"candidate_pixels\":"<<result.candidate_pixels<<",\"components\":"<<result.components<<",\"border_abstentions\":"<<result.border_abstentions<<"}\n";
  report.close();
  if(!report)throw std::runtime_error("Relatorio de perfil nao foi gravado");
  return result;
 }catch(...){std::error_code ec;fs::remove_all(out_dir,ec);throw;}
}
}
