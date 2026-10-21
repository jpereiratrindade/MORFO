#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace morfo::evaluate {

struct BinaryMetrics {
 std::uint64_t true_positive{},true_negative{},false_positive{},false_negative{};
 double precision{},recall{},f1{},accuracy{},dice{},iou{};
};

struct MeasurementMetrics {double mae{},rmse{},bias{};std::size_t count{};};
struct Interval {double estimate{},lower{},upper{};std::size_t samples{};};

// Prediction and reference must be independent, equally-sized binary masks.
BinaryMetrics binary(std::span<const std::uint8_t> prediction,
                     std::span<const std::uint8_t> reference);
MeasurementMetrics measurement(std::span<const double> prediction,
                               std::span<const double> reference);

// Label names are irrelevant. Returns 1 for identical partitions, including
// partitions whose numeric labels differ. Undefined degenerate pairs return 1.
double adjusted_rand_index(std::span<const std::int64_t> discovered,
                           std::span<const std::int64_t> reference);

// Euclidean silhouette. Singletons contribute zero. At least two clusters required.
double silhouette(std::span<const double> row_major_points,std::size_t dimensions,
                  std::span<const std::int64_t> labels);

// Deterministic percentile bootstrap over per-unit scores. Experimental units,
// never image crops, are the resampling unit.
Interval bootstrap_mean_95(std::span<const double> unit_scores,
                           std::size_t resamples=2000,std::uint64_t seed=0x4d4f52464fULL);

} // namespace morfo::evaluate
