#pragma once

#include <cstdint>
#include <vector>

namespace rizoma::analysis {

struct Region {
 std::uint64_t x{},y{},width{},height{},area{};
 std::uint64_t global_pixels{},local_pixels{},shared_pixels{};
 bool touches_border{};
};

struct Result {
 std::vector<Region> regions;
 double global_mean{},global_stddev{};
};

// Experimental in-memory baseline derived from EXP-002/003 candidate concepts.
// A candidate region is not a pixel-validated root segmentation.
Result detect(std::uint64_t width,std::uint64_t height,const std::vector<unsigned char>& rgba);

} // namespace rizoma::analysis
