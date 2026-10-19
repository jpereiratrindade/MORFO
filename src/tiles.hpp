#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rizoma::tiles {

struct Level {std::uint64_t width{},height{};};
struct Pyramid {
 std::uint64_t width{},height{};
 std::uint32_t tile{};
 std::string source_sha256,source_format,preview_transform;
 std::vector<Level> levels; // L0 is the original resolution.
};

// EXP-004: the source is decoded progressively. Working memory is bounded by
// scanline/tile bands rather than by the total image area.
Pyramid create(const std::filesystem::path& source,const std::filesystem::path& directory,std::uint32_t tile=256);

} // namespace rizoma::tiles
