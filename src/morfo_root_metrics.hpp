#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace morfo::root_metrics {

inline constexpr const char* algorithm_version="morfo-root-metrics-001b-0.2.0";
inline constexpr const char* scientific_status="NOT_VALIDATED";
inline constexpr const char* result_nature=
 "Morfometria bidimensional de candidatos radiculares detectados pelo EXP-003";

struct RasterMask {
 std::size_t width=0;
 std::size_t height=0;
 std::vector<std::uint8_t> pixels;
};

struct Calibration {
 std::optional<double> scale_x_mm_per_pixel;
 std::optional<double> scale_y_mm_per_pixel;
 std::string origin;
 std::string method;
 std::string date;
 std::string version;
 std::string responsible;
};

struct BoundingBox {
 std::size_t min_x=0;
 std::size_t min_y=0;
 std::size_t max_x=0;
 std::size_t max_y=0;
};

struct ComponentMetrics {
 std::uint64_t id=0;
 std::uint64_t area_px=0;
 std::optional<double> area_mm2;
 std::optional<double> area_cm2;
 double centroid_x_px=0.0;
 double centroid_y_px=0.0;
 BoundingBox bounding_box;
 bool touches_image_boundary=false;
 bool touches_roi_boundary=false;
 bool touches_abstained_area=false;
 std::uint64_t skeleton_pixels=0;
 std::uint64_t graph_edges=0;
 double projected_length_px=0.0;
 std::optional<double> projected_length_mm;
 std::optional<double> projected_length_cm;
};

struct Input {
 RasterMask mask;
 RasterMask roi;
 RasterMask valid_mask;
 Calibration calibration;
 std::string mask_run_id;
 std::string mask_sha256;
 std::string profile="roots-exp003";
 std::string profile_version;
 std::string profile_parameters_json;
};

struct Result {
 std::size_t width=0;
 std::size_t height=0;
 std::uint64_t roi_pixels=0;
 std::uint64_t valid_pixels=0;
 std::uint64_t candidate_pixels=0;
 std::uint64_t matrix_pixels=0;
 std::uint64_t excluded_or_abstained_pixels=0;
 std::optional<double> occupancy_percent;
 std::optional<double> projected_area_mm2;
 std::optional<double> projected_area_cm2;
 std::uint64_t skeleton_pixels=0;
 std::uint64_t graph_edges=0;
 double projected_length_px=0.0;
 std::optional<double> projected_length_mm;
 std::optional<double> projected_length_cm;
 std::vector<ComponentMetrics> components;
 Calibration calibration;
 std::string mask_run_id;
 std::string mask_sha256;
 std::string profile;
 std::string profile_version;
 std::string profile_parameters_json;
 std::string metrics_version=algorithm_version;
 std::string status=scientific_status;
 std::string nature=result_nature;
};

// Computes M01 through M04. M04 uses deterministic Guo-Hall thinning and an
// 8-neighbor graph with redundant diagonals removed.
// Throws std::invalid_argument for malformed dimensions or calibration.
Result compute_gate_001a(const Input& input);

}
