#include "morfo_pre.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace morfo::pre {
namespace {

double distance(const Descriptor& a, const Descriptor& b) {
 double sum = 0;
 for (std::size_t i = 0; i < a.values.size(); ++i) {
  const double delta = a.values[i] - b.values[i];
  sum += delta * delta;
 }
 return std::sqrt(sum);
}

Descriptor describe(std::size_t width, const std::vector<unsigned char>& rgba,
                    std::size_t x0, std::size_t y0, std::size_t w, std::size_t h) {
 Descriptor descriptor;
 double luminance = 0, luminance_squared = 0, edges = 0;
 const double count = static_cast<double>(w * h);
 for (std::size_t y = y0; y < y0 + h; ++y) {
  for (std::size_t x = x0; x < x0 + w; ++x) {
   const auto i = (y * width + x) * 4;
   const double red = rgba[i] / 255.0;
   const double green = rgba[i + 1] / 255.0;
   const double blue = rgba[i + 2] / 255.0;
   const double value = .2126 * red + .7152 * green + .0722 * blue;
   descriptor.values[0] += red;
   descriptor.values[1] += green;
   descriptor.values[2] += blue;
   luminance += value;
   luminance_squared += value * value;
   if (x > x0) {
    const auto j = i - 4;
    edges += std::abs(value - (.2126 * rgba[j] / 255.0 + .7152 * rgba[j + 1] / 255.0 + .0722 * rgba[j + 2] / 255.0));
   }
   if (y > y0) {
    const auto j = i - width * 4;
    edges += std::abs(value - (.2126 * rgba[j] / 255.0 + .7152 * rgba[j + 1] / 255.0 + .0722 * rgba[j + 2] / 255.0));
   }
  }
 }
 descriptor.values[0] /= count;
 descriptor.values[1] /= count;
 descriptor.values[2] /= count;
 descriptor.values[3] = luminance / count;
 descriptor.values[4] = std::sqrt(std::max(0.0, luminance_squared / count - descriptor.values[3] * descriptor.values[3]));
 descriptor.values[5] = edges / std::max(1.0, 2 * count - w - h);
 return descriptor;
}

std::pair<std::size_t, double> nearest_prototype(const Descriptor& descriptor,
                                                 const std::vector<Descriptor>& prototypes) {
 std::size_t best = 0;
 double nearest = distance(descriptor, prototypes.front());
 for (std::size_t cluster = 1; cluster < prototypes.size(); ++cluster) {
  const double candidate = distance(descriptor, prototypes[cluster]);
  if (candidate < nearest) {
   nearest = candidate;
   best = cluster;
  }
 }
 return {best, nearest};
}

} // namespace

Result perceive(std::size_t width, std::size_t height,
                const std::vector<unsigned char>& rgba, Config config) {
 if (!width || !height || rgba.size() != width * height * 4)
  throw std::invalid_argument("PRE-001 requires an RGBA image");
 if (config.cell < 4 || config.clusters < 1 || config.iterations < 1)
  throw std::invalid_argument("Invalid PRE-001 configuration");

 Result result;
 result.width = width;
 result.height = height;
 result.columns = (width + config.cell - 1) / config.cell;
 result.rows = (height + config.cell - 1) / config.cell;
 for (std::size_t y = 0; y < height; y += config.cell) {
  for (std::size_t x = 0; x < width; x += config.cell) {
   const auto cell_width = std::min(config.cell, width - x);
   const auto cell_height = std::min(config.cell, height - y);
   result.cells.push_back({x, y, cell_width, cell_height, 0, 0, 0,
                           describe(width, rgba, x, y, cell_width, cell_height)});
  }
 }

 const auto requested_clusters = std::min(config.clusters, result.cells.size());
 result.prototypes.push_back(result.cells.front().descriptor);
 while (result.prototypes.size() < requested_clusters) {
  std::size_t best = 0;
  double farthest = -1;
  for (std::size_t i = 0; i < result.cells.size(); ++i) {
   const auto [_, nearest] = nearest_prototype(result.cells[i].descriptor, result.prototypes);
   if (nearest > farthest) {
    farthest = nearest;
    best = i;
   }
  }
  if (farthest <= 1e-12) break;
  result.prototypes.push_back(result.cells[best].descriptor);
 }

 const auto cluster_count = result.prototypes.size();
 for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
  bool changed = false;
  std::vector<Descriptor> sums(cluster_count);
  std::vector<std::size_t> counts(cluster_count);
  for (auto& cell : result.cells) {
   const auto [best, nearest] = nearest_prototype(cell.descriptor, result.prototypes);
   changed |= cell.cluster != best;
   cell.cluster = best;
   cell.distance = nearest;
   ++counts[best];
   for (std::size_t feature = 0; feature < cell.descriptor.values.size(); ++feature)
    sums[best].values[feature] += cell.descriptor.values[feature];
  }
  for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
   if (!counts[cluster]) continue;
   for (std::size_t feature = 0; feature < result.prototypes[cluster].values.size(); ++feature)
    result.prototypes[cluster].values[feature] = sums[cluster].values[feature] / counts[cluster];
  }
  if (!changed && iteration) break;
 }

 std::vector<double> mean(cluster_count), spread(cluster_count);
 std::vector<std::size_t> counts(cluster_count);
 for (auto& cell : result.cells) {
  const auto [best, nearest] = nearest_prototype(cell.descriptor, result.prototypes);
  cell.cluster = best;
  cell.distance = nearest;
  mean[best] += nearest;
  ++counts[best];
 }
 for (std::size_t cluster = 0; cluster < cluster_count; ++cluster)
  if (counts[cluster]) mean[cluster] /= counts[cluster];
 for (const auto& cell : result.cells) {
  const double delta = cell.distance - mean[cell.cluster];
  spread[cell.cluster] += delta * delta;
 }
 for (std::size_t cluster = 0; cluster < cluster_count; ++cluster)
  spread[cluster] = counts[cluster] > 1 ? std::sqrt(spread[cluster] / counts[cluster]) : 0;
 for (auto& cell : result.cells)
  cell.novelty = spread[cell.cluster] > 1e-9
   ? std::max(0.0, (cell.distance - mean[cell.cluster]) / spread[cell.cluster]) : 0;
 return result;
}

} // namespace morfo::pre
