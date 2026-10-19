#include "morfo_root_metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace morfo::root_metrics {
namespace {

std::size_t checked_size(std::size_t width,std::size_t height){
 if(width==0||height==0)throw std::invalid_argument("root metrics dimensions must be positive");
 if(width>std::numeric_limits<std::size_t>::max()/height)
  throw std::invalid_argument("root metrics dimensions overflow");
 return width*height;
}

void validate_raster(const RasterMask& raster,std::size_t width,std::size_t height,const char* name){
 if(raster.width!=width||raster.height!=height||raster.pixels.size()!=checked_size(width,height))
  throw std::invalid_argument(std::string(name)+" dimensions do not match mask");
}

std::optional<double> pixel_area(const Calibration& calibration){
 const bool has_x=calibration.scale_x_mm_per_pixel.has_value();
 const bool has_y=calibration.scale_y_mm_per_pixel.has_value();
 if(has_x!=has_y)throw std::invalid_argument("calibration requires both X and Y scales");
 if(!has_x)return std::nullopt;
 const double x=*calibration.scale_x_mm_per_pixel;
 const double y=*calibration.scale_y_mm_per_pixel;
 if(!std::isfinite(x)||!std::isfinite(y)||x<=0.0||y<=0.0)
  throw std::invalid_argument("calibration scales must be finite and positive");
 return x*y;
}

bool active(const Input& input,std::size_t index){
 return input.mask.pixels[index]!=0&&input.roi.pixels[index]!=0&&input.valid_mask.pixels[index]!=0;
}

}

Result compute_gate_001a(const Input& input){
 const std::size_t total=checked_size(input.mask.width,input.mask.height);
 if(input.mask.pixels.size()!=total)throw std::invalid_argument("mask dimensions do not match pixel storage");
 validate_raster(input.roi,input.mask.width,input.mask.height,"roi");
 validate_raster(input.valid_mask,input.mask.width,input.mask.height,"valid_mask");
 const auto area_per_pixel=pixel_area(input.calibration);

 Result result;
 result.width=input.mask.width;
 result.height=input.mask.height;
 result.calibration=input.calibration;
 result.mask_run_id=input.mask_run_id;
 result.mask_sha256=input.mask_sha256;
 result.profile=input.profile;
 result.profile_version=input.profile_version;
 result.profile_parameters_json=input.profile_parameters_json;

 for(std::size_t i=0;i<total;++i){
  if(input.roi.pixels[i]==0)continue;
  ++result.roi_pixels;
  if(input.valid_mask.pixels[i]==0){++result.excluded_or_abstained_pixels;continue;}
  ++result.valid_pixels;
  if(input.mask.pixels[i]!=0)++result.candidate_pixels;
  else ++result.matrix_pixels;
 }
 if(result.valid_pixels!=0)
  result.occupancy_percent=100.0*static_cast<double>(result.candidate_pixels)/static_cast<double>(result.valid_pixels);
 if(area_per_pixel){
  result.projected_area_mm2=static_cast<double>(result.candidate_pixels)**area_per_pixel;
  result.projected_area_cm2=*result.projected_area_mm2/100.0;
 }

 std::vector<std::uint8_t> visited(total,0);
 constexpr std::array<std::pair<int,int>,8> neighbors{{
  {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}
 }};
 for(std::size_t seed=0;seed<total;++seed){
  if(visited[seed]!=0||!active(input,seed))continue;
  ComponentMetrics component;
  component.id=result.components.size()+1;
  const std::size_t seed_x=seed%input.mask.width;
  const std::size_t seed_y=seed/input.mask.width;
  component.bounding_box={seed_x,seed_y,seed_x,seed_y};
  std::uint64_t sum_x=0,sum_y=0;
  std::queue<std::size_t> pending;
  pending.push(seed);visited[seed]=1;
  while(!pending.empty()){
   const std::size_t index=pending.front();pending.pop();
   const std::size_t x=index%input.mask.width,y=index/input.mask.width;
   ++component.area_px;sum_x+=x;sum_y+=y;
   component.bounding_box.min_x=std::min(component.bounding_box.min_x,x);
   component.bounding_box.min_y=std::min(component.bounding_box.min_y,y);
   component.bounding_box.max_x=std::max(component.bounding_box.max_x,x);
   component.bounding_box.max_y=std::max(component.bounding_box.max_y,y);
   if(x==0||y==0||x+1==input.mask.width||y+1==input.mask.height)
    component.touches_image_boundary=true;
   for(const auto& [dx,dy]:neighbors){
    const auto nx=static_cast<std::ptrdiff_t>(x)+dx;
    const auto ny=static_cast<std::ptrdiff_t>(y)+dy;
    if(nx<0||ny<0||nx>=static_cast<std::ptrdiff_t>(input.mask.width)||ny>=static_cast<std::ptrdiff_t>(input.mask.height)){
     component.touches_roi_boundary=true;
     continue;
    }
    const auto neighbor=static_cast<std::size_t>(ny)*input.mask.width+static_cast<std::size_t>(nx);
    if(input.roi.pixels[neighbor]==0){component.touches_roi_boundary=true;continue;}
    if(input.valid_mask.pixels[neighbor]==0){component.touches_abstained_area=true;continue;}
    if(input.mask.pixels[neighbor]!=0&&visited[neighbor]==0){visited[neighbor]=1;pending.push(neighbor);}
   }
  }
  component.centroid_x_px=static_cast<double>(sum_x)/static_cast<double>(component.area_px);
  component.centroid_y_px=static_cast<double>(sum_y)/static_cast<double>(component.area_px);
  if(area_per_pixel){
   component.area_mm2=static_cast<double>(component.area_px)**area_per_pixel;
   component.area_cm2=*component.area_mm2/100.0;
  }
  result.components.push_back(component);
 }
 return result;
}

}
