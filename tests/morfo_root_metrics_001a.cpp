#include "morfo_root_metrics.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace morfo::root_metrics;

namespace {
RasterMask raster(std::size_t width,std::size_t height,std::uint8_t value=0){
 return {width,height,std::vector<std::uint8_t>(width*height,value)};
}
void set(RasterMask& value,std::size_t x,std::size_t y){value.pixels[y*value.width+x]=1;}
Input basic(std::size_t width,std::size_t height){
 return {raster(width,height),raster(width,height,1),raster(width,height,1),{},"run-1","sha-1","roots-exp003","exp003-original-0.1.0","{}",{}};
}
bool close(double left,double right){return std::abs(left-right)<1e-10;}
}

int main(){
 {
  const auto result=compute_gate_001a(basic(5,4));
  assert(result.candidate_pixels==0&&result.valid_pixels==20&&result.matrix_pixels==20);
  assert(result.components.empty()&&result.occupancy_percent&&close(*result.occupancy_percent,0.0));
  assert(!result.projected_area_mm2);
 }
 {
  auto input=basic(12,12);
  for(std::size_t y=1;y<=10;++y)for(std::size_t x=1;x<=10;++x)set(input.mask,x,y);
  input.calibration.scale_x_mm_per_pixel=0.5;
  input.calibration.scale_y_mm_per_pixel=0.25;
  input.calibration.method="manual";
  const auto result=compute_gate_001a(input);
  assert(result.candidate_pixels==100&&result.components.size()==1);
  assert(result.projected_area_mm2&&close(*result.projected_area_mm2,12.5));
  assert(result.projected_area_cm2&&close(*result.projected_area_cm2,0.125));
  const auto& component=result.components.front();
  assert(component.id==1&&component.area_px==100);
  assert(close(component.centroid_x_px,5.5)&&close(component.centroid_y_px,5.5));
  assert(component.bounding_box.min_x==1&&component.bounding_box.min_y==1);
  assert(component.bounding_box.max_x==10&&component.bounding_box.max_y==10);
  assert(!component.touches_image_boundary&&!component.touches_roi_boundary&&!component.touches_abstained_area);
 }
 {
  auto input=basic(8,5);
  set(input.mask,1,1);set(input.mask,2,2); // One component under 8-connectivity.
  set(input.mask,6,3);
  const auto result=compute_gate_001a(input);
  assert(result.components.size()==2);
  assert(result.candidate_pixels==3);
  assert(result.components[0].id==1&&result.components[0].area_px==2);
  assert(result.components[1].id==2&&result.components[1].area_px==1);
  assert(result.components[0].area_px+result.components[1].area_px==result.candidate_pixels);
 }
 {
  auto input=basic(5,5);
  input.roi=raster(5,5,0);
  for(std::size_t y=1;y<4;++y)for(std::size_t x=1;x<4;++x)set(input.roi,x,y);
  set(input.mask,1,2);set(input.mask,0,2); // Pixel outside ROI is excluded.
  input.valid_mask.pixels[2*5+2]=0;
  const auto result=compute_gate_001a(input);
  assert(result.roi_pixels==9&&result.valid_pixels==8);
  assert(result.excluded_or_abstained_pixels==1&&result.candidate_pixels==1&&result.matrix_pixels==7);
  assert(result.components.size()==1);
  assert(result.components[0].touches_roi_boundary);
  assert(result.components[0].touches_abstained_area);
  assert(!result.components[0].touches_image_boundary);
  assert(result.occupancy_percent&&close(*result.occupancy_percent,12.5));
 }
 {
  auto input=basic(3,3);
  input.valid_mask=raster(3,3,0);
  set(input.mask,1,1);
  const auto result=compute_gate_001a(input);
  assert(result.valid_pixels==0&&result.excluded_or_abstained_pixels==9);
  assert(!result.occupancy_percent&&result.components.empty());
 }
 {
  auto input=basic(3,3);set(input.mask,0,0);
  const auto result=compute_gate_001a(input);
  assert(result.components.front().touches_image_boundary);
  assert(result.components.front().touches_roi_boundary);
 }
 {
  auto input=basic(2,2);input.calibration.scale_x_mm_per_pixel=1.0;
  bool threw=false;try{(void)compute_gate_001a(input);}catch(const std::invalid_argument&){threw=true;}
  assert(threw);
  input.calibration.scale_y_mm_per_pixel=-1.0;
  threw=false;try{(void)compute_gate_001a(input);}catch(const std::invalid_argument&){threw=true;}
  assert(threw);
 }
 {
  auto input=basic(13,3);for(std::size_t x=1;x<=11;++x)set(input.mask,x,1);
  const auto result=compute_gate_001a(input);
  assert(result.skeleton_pixels==11&&result.graph_edges==10);
  assert(close(result.projected_length_px,10.0));
  assert(result.components[0].skeleton_pixels==11&&close(result.components[0].projected_length_px,10.0));
 }
 {
  auto input=basic(3,13);for(std::size_t y=1;y<=11;++y)set(input.mask,1,y);
  input.calibration.scale_x_mm_per_pixel=0.5;input.calibration.scale_y_mm_per_pixel=2.0;
  const auto result=compute_gate_001a(input);
  assert(close(result.projected_length_px,10.0));
  assert(result.projected_length_mm&&close(*result.projected_length_mm,20.0));
  assert(result.projected_length_cm&&close(*result.projected_length_cm,2.0));
 }
 {
  auto input=basic(13,13);for(std::size_t i=1;i<=11;++i)set(input.mask,i,i);
  const auto result=compute_gate_001a(input);
  assert(result.graph_edges==10&&close(result.projected_length_px,10.0*std::sqrt(2.0)));
 }
 {
  auto input=basic(7,7);for(std::size_t x=1;x<=5;++x){set(input.mask,x,3);set(input.mask,x,4);}
  const auto result=compute_gate_001a(input);
  // Orthogonal intermediate pixels suppress redundant diagonal graph edges.
  assert(result.projected_length_px<=6.0);
 }
 {
  auto input=basic(8,5);for(std::size_t x=0;x<8;++x)set(input.mask,x,2);
  input.roi=raster(8,5,0);for(std::size_t y=0;y<5;++y)for(std::size_t x=2;x<=6;++x)set(input.roi,x,y);
  const auto result=compute_gate_001a(input);
  assert(result.candidate_pixels==5&&close(result.projected_length_px,4.0));
  assert(result.components.size()==1&&result.components[0].touches_roi_boundary);
 }
 {
  auto input=basic(7,8);for(std::size_t x=1;x<=5;++x)set(input.mask,x,2);for(std::size_t y=2;y<=6;++y)set(input.mask,3,y);
  const auto result=compute_gate_001a(input);
  assert(result.apparent_tips==3);
  assert(result.apparent_branches==1);
  assert(result.branch_incident_arms.size()==1&&result.branch_incident_arms[0]==3);
 }
 {
  auto input=basic(5,5);set(input.mask,2,2);
  const auto result=compute_gate_001a(input);
  assert(result.isolated_skeleton_pixels==1&&result.apparent_tips==0&&result.apparent_branches==0);
 }
 {
  auto input=basic(7,7);for(std::size_t x=2;x<=4;++x){set(input.mask,x,2);set(input.mask,x,4);}set(input.mask,2,3);set(input.mask,4,3);
  const auto result=compute_gate_001a(input);
  assert(result.apparent_tips==0);
 }
 {
  auto input=basic(9,9);for(std::size_t y=3;y<=5;++y)for(std::size_t x=1;x<=7;++x)set(input.mask,x,y);
  input.calibration.scale_x_mm_per_pixel=0.5;input.calibration.scale_y_mm_per_pixel=0.5;
  const auto result=compute_gate_001a(input);
  assert(result.mean_diameter&&close(*result.mean_diameter,2.0));
  assert(result.min_diameter&&close(*result.min_diameter,2.0));
  assert(result.max_diameter&&close(*result.max_diameter,2.0));
  assert(close(result.measurable_diameter_length_fraction,1.0));
  assert(result.diameter_unit=="mm"&&result.diameter_classes.size()==4);
  assert(close(result.diameter_classes[3].percent,100.0));
 }
 {
  auto input=basic(9,9);for(std::size_t y=3;y<=5;++y)for(std::size_t x=0;x<=7;++x)set(input.mask,x,y);
  const auto result=compute_gate_001a(input);
  assert(!result.mean_diameter&&close(result.measurable_diameter_length_fraction,0.0));
 }
}
