#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace morfo::pre {
inline constexpr const char* version="pre001-0.1.0-experimental";
struct Config {std::size_t cell=32,clusters=3,iterations=24;};
struct Descriptor {std::array<double,8> values{};};
struct Cell {std::size_t x=0,y=0,width=0,height=0,cluster=0;double distance=0,novelty=0;Descriptor descriptor;};
struct Result {std::size_t width=0,height=0,columns=0,rows=0;std::vector<Descriptor> prototypes;std::vector<Cell> cells;};
Result perceive(std::size_t width,std::size_t height,const std::vector<unsigned char>& rgba,Config config={});
}
