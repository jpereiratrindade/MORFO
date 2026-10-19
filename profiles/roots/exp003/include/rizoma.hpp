#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace rizoma {
namespace fs = std::filesystem;
struct Rgb {std::uint8_t r=0,g=0,b=0; bool operator==(const Rgb&) const = default;};
struct Image {int width=0,height=0;std::vector<Rgb> pixels;};
struct Gray {int width=0,height=0;std::vector<std::uint8_t> pixels;};
struct Component {int area=0, min_x=0, min_y=0, max_x=0, max_y=0; double elongation=0.0;};
struct Summary {std::size_t pixels=0, components=0, bottom10=0;std::vector<int> largest;};
struct Params {int top_hat_radius=8, cell=32, context=64, border=10; double seed_z=2.4, min_tophat=7.0; int min_component_area=4;};
Image read_rgb(const fs::path& p);
void write_png(const fs::path& p,const Image& img);
void write_png(const fs::path& p,const Gray& img);
std::string sha256_file(const fs::path& p);
Image negative(const Image& img);
Image overlay(const Image& img,const Gray& mask);
Gray grayscale(const Image& img);
Gray tophat(const Gray& gray,int radius);
Gray legacy_score(const Image& img,const Gray& g,const Gray& th);
Gray legacy_mask(const Image& img,const Gray& g,const Gray& th);
struct Adaptive {Gray zview, mask;std::size_t border_abstentions=0;};
Adaptive adaptive_detect(const Image& img,const Gray& g,const Gray& th,const Params& opts);
std::vector<Component> components(const Gray& mask);
Summary summarize(const Gray& mask);
std::size_t overlap(const Gray& left,const Gray& right);
// EXP-003: only records consensus/discordance. No biological identity is asserted.
struct ComparisonCounts {
 std::size_t neither=0, shared=0, legacy_only=0, adaptive_only=0;
 [[nodiscard]] std::size_t total() const { return neither+shared+legacy_only+adaptive_only; }
};
struct ReviewRegion {
 int id=0, x_min=0, y_min=0, x_max=0, y_max=0;
 std::size_t area=0, shared=0, legacy_only=0, adaptive_only=0;
 bool touches_border=false;
};
struct Comparison {
 ComparisonCounts counts;
 Image visualization;
 Gray class_map; // 0 neither; 85 only legacy; 170 only adaptive; 255 both
 std::vector<ReviewRegion> regions;
};
Comparison compare_methods(const Image& original, const Gray& legacy, const Gray& adaptive,
                           int review_border = 10);

}
