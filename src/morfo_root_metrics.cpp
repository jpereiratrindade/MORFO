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

std::vector<std::uint8_t> guo_hall(std::vector<std::uint8_t> image,std::size_t width,std::size_t height){
 if(width<3||height<3)return image;
 bool changed=true;std::vector<std::size_t> remove;
 auto at=[&](std::ptrdiff_t x,std::ptrdiff_t y){return image[static_cast<std::size_t>(y)*width+static_cast<std::size_t>(x)]!=0;};
 while(changed){changed=false;
  for(int pass=0;pass<2;++pass){remove.clear();
   for(std::size_t y=1;y+1<height;++y)for(std::size_t x=1;x+1<width;++x){const auto index=y*width+x;if(image[index]==0)continue;
    const bool p2=at(x,y-1),p3=at(x+1,y-1),p4=at(x+1,y),p5=at(x+1,y+1),p6=at(x,y+1),p7=at(x-1,y+1),p8=at(x-1,y),p9=at(x-1,y-1);
    const int transitions=(!p2&&(p3||p4))+(!p4&&(p5||p6))+(!p6&&(p7||p8))+(!p8&&(p9||p2));
    const int n1=(p9||p2)+(p3||p4)+(p5||p6)+(p7||p8),n2=(p2||p3)+(p4||p5)+(p6||p7)+(p8||p9);
    const int neighbors_count=std::min(n1,n2);
    const bool preserve=pass==0?((p6||p7||!p9)&&p8):((p2||p3||!p5)&&p4);
    if(transitions==1&&neighbors_count>=2&&neighbors_count<=3&&!preserve)remove.push_back(index);
   }
   if(!remove.empty()){changed=true;for(const auto index:remove)image[index]=0;}
  }
 }
 return image;
}

std::vector<double> edt_1d(const std::vector<double>& f,double spacing){
 const std::size_t n=f.size();const double inf=std::numeric_limits<double>::infinity(),a=spacing*spacing;
 std::vector<double> d(n,inf);if(n==0)return d;std::vector<std::size_t> sites;sites.reserve(n);for(std::size_t i=0;i<n;++i)if(std::isfinite(f[i]))sites.push_back(i);if(sites.empty())return d;
 std::vector<std::size_t> v(sites.size());std::vector<double> z(sites.size()+1);std::size_t k=0;v[0]=sites[0];z[0]=-inf;z[1]=inf;
 for(std::size_t si=1;si<sites.size();++si){const auto q=sites[si];double s=0;while(true){const auto p=v[k];s=((f[q]+a*double(q)*double(q))-(f[p]+a*double(p)*double(p)))/(2.0*a*double(q-p));if(s>z[k]||k==0)break;--k;}if(s<=z[k]&&k==0){v[0]=q;z[1]=inf;}else{++k;v[k]=q;z[k]=s;z[k+1]=inf;}}
 k=0;for(std::size_t q=0;q<n;++q){while(z[k+1]<double(q))++k;const double delta=double(q)-double(v[k]);d[q]=f[v[k]]+a*delta*delta;}return d;
}

std::vector<double> euclidean_distance_squared(const RasterMask& mask,double sx,double sy){
 const double inf=std::numeric_limits<double>::infinity();std::vector<double> horizontal(mask.pixels.size(),inf),f(mask.width);
 for(std::size_t y=0;y<mask.height;++y){for(std::size_t x=0;x<mask.width;++x)f[x]=mask.pixels[y*mask.width+x]?inf:0.0;const auto row=edt_1d(f,sx);for(std::size_t x=0;x<mask.width;++x)horizontal[y*mask.width+x]=row[x];}
 std::vector<double> result(mask.pixels.size(),inf);f.resize(mask.height);for(std::size_t x=0;x<mask.width;++x){for(std::size_t y=0;y<mask.height;++y)f[y]=horizontal[y*mask.width+x];const auto column=edt_1d(f,sy);for(std::size_t y=0;y<mask.height;++y)result[y*mask.width+x]=column[y];}return result;
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

 std::vector<std::uint8_t> binary(total,0);for(std::size_t i=0;i<total;++i)binary[i]=active(input,i)?1:0;
 const auto skeleton=guo_hall(std::move(binary),input.mask.width,input.mask.height);
 std::vector<std::size_t> component_by_pixel(total,std::numeric_limits<std::size_t>::max());
 for(std::size_t index=0;index<total;++index)if(skeleton[index]){
  ++result.skeleton_pixels;
  // Stable component assignment: bounding-box candidates are disambiguated by flood labels below.
 }
 std::vector<std::uint8_t> labeled(total,0);std::size_t component_index=0;
 for(std::size_t seed=0;seed<total;++seed){if(labeled[seed]||!active(input,seed))continue;std::queue<std::size_t> pending;pending.push(seed);labeled[seed]=1;
  while(!pending.empty()){const auto index=pending.front();pending.pop();if(skeleton[index])component_by_pixel[index]=component_index;const auto x=index%input.mask.width,y=index/input.mask.width;
   for(const auto& [dx,dy]:neighbors){const auto nx=static_cast<std::ptrdiff_t>(x)+dx,ny=static_cast<std::ptrdiff_t>(y)+dy;if(nx<0||ny<0||nx>=static_cast<std::ptrdiff_t>(input.mask.width)||ny>=static_cast<std::ptrdiff_t>(input.mask.height))continue;const auto next=static_cast<std::size_t>(ny)*input.mask.width+static_cast<std::size_t>(nx);if(!labeled[next]&&active(input,next)){labeled[next]=1;pending.push(next);}}
  }++component_index;
 }
 struct GraphEdge{std::size_t first,second,owner;double length_px,length_measure;};std::vector<GraphEdge> graph;
 constexpr std::array<std::pair<int,int>,4> forward{{{1,0},{0,1},{1,1},{-1,1}}};
 const double sx=input.calibration.scale_x_mm_per_pixel.value_or(1.0),sy=input.calibration.scale_y_mm_per_pixel.value_or(1.0);
 std::vector<std::uint8_t> degree(total,0);
 auto edge_allowed=[&](std::size_t index,std::size_t next,int dx,int dy){
  if(dx!=0&&dy!=0){const auto x=index%input.mask.width,y=index/input.mask.width,nx=next%input.mask.width,ny=next/input.mask.width;const auto middle_x=y*input.mask.width+nx,middle_y=ny*input.mask.width+x;if(skeleton[middle_x]||skeleton[middle_y])return false;}
  return true;
 };
 for(std::size_t y=0;y<input.mask.height;++y)for(std::size_t x=0;x<input.mask.width;++x){const auto index=y*input.mask.width+x;if(!skeleton[index])continue;const auto owner=component_by_pixel[index];if(owner<result.components.size())++result.components[owner].skeleton_pixels;
  for(const auto& [dx,dy]:forward){const auto nx=static_cast<std::ptrdiff_t>(x)+dx,ny=static_cast<std::ptrdiff_t>(y)+dy;if(nx<0||ny<0||nx>=static_cast<std::ptrdiff_t>(input.mask.width)||ny>=static_cast<std::ptrdiff_t>(input.mask.height))continue;const auto next=static_cast<std::size_t>(ny)*input.mask.width+static_cast<std::size_t>(nx);if(!skeleton[next])continue;
   if(!edge_allowed(index,next,dx,dy))continue;
   const double px=std::hypot(static_cast<double>(dx),static_cast<double>(dy)),mm=std::hypot(dx*sx,dy*sy);++result.graph_edges;result.projected_length_px+=px;if(area_per_pixel)result.projected_length_mm=result.projected_length_mm.value_or(0.0)+mm;
   graph.push_back({index,next,owner,px,area_per_pixel?mm:px});
   if(degree[index]<255)++degree[index];
   if(degree[next]<255)++degree[next];
   if(owner<result.components.size()){auto& component=result.components[owner];++component.graph_edges;component.projected_length_px+=px;if(area_per_pixel)component.projected_length_mm=component.projected_length_mm.value_or(0.0)+mm;}
  }
 }
 if(result.projected_length_mm)result.projected_length_cm=*result.projected_length_mm/10.0;
 for(auto& component:result.components)if(component.projected_length_mm)component.projected_length_cm=*component.projected_length_mm/10.0;
 for(std::size_t index=0;index<total;++index)if(skeleton[index]){const auto owner=component_by_pixel[index];if(degree[index]==1){++result.apparent_tips;if(owner<result.components.size()){++result.components[owner].apparent_tips;if(result.components[owner].touches_image_boundary||result.components[owner].touches_roi_boundary||result.components[owner].touches_abstained_area)++result.truncated_tips;}}else if(degree[index]==0){++result.isolated_skeleton_pixels;if(owner<result.components.size())++result.components[owner].isolated_skeleton_pixels;}}
 std::vector<std::uint8_t> branch_seen(total,0);constexpr std::array<std::pair<int,int>,8> all_directions{{{-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1}}};
 for(std::size_t seed=0;seed<total;++seed){if(!skeleton[seed]||degree[seed]<3||branch_seen[seed])continue;std::queue<std::size_t> pending;std::vector<std::size_t> region;pending.push(seed);branch_seen[seed]=1;
  while(!pending.empty()){const auto index=pending.front();pending.pop();region.push_back(index);const auto x=index%input.mask.width,y=index/input.mask.width;for(const auto& [dx,dy]:all_directions){const auto nx=static_cast<std::ptrdiff_t>(x)+dx,ny=static_cast<std::ptrdiff_t>(y)+dy;if(nx<0||ny<0||nx>=static_cast<std::ptrdiff_t>(input.mask.width)||ny>=static_cast<std::ptrdiff_t>(input.mask.height))continue;const auto next=static_cast<std::size_t>(ny)*input.mask.width+static_cast<std::size_t>(nx);if(skeleton[next]&&degree[next]>=3&&!branch_seen[next]&&edge_allowed(index,next,dx,dy)){branch_seen[next]=1;pending.push(next);}}}
  std::uint64_t arms=0;for(const auto index:region){const auto x=index%input.mask.width,y=index/input.mask.width;for(const auto& [dx,dy]:all_directions){const auto nx=static_cast<std::ptrdiff_t>(x)+dx,ny=static_cast<std::ptrdiff_t>(y)+dy;if(nx<0||ny<0||nx>=static_cast<std::ptrdiff_t>(input.mask.width)||ny>=static_cast<std::ptrdiff_t>(input.mask.height))continue;const auto next=static_cast<std::size_t>(ny)*input.mask.width+static_cast<std::size_t>(nx);if(skeleton[next]&&degree[next]<3&&edge_allowed(index,next,dx,dy))++arms;}}
  ++result.apparent_branches;result.branch_incident_arms.push_back(arms);const auto owner=component_by_pixel[seed];if(owner<result.components.size())++result.components[owner].apparent_branches;
 }
 const auto edt=euclidean_distance_squared(input.mask,sx,sy);double weighted_diameter=0.0,measurable_length=0.0,min_diameter=std::numeric_limits<double>::infinity(),max_diameter=0.0,total_measure_length=0.0;
 std::vector<double> bins=input.diameter_bin_upper_bounds;if(bins.empty()&&area_per_pixel)bins={0.5,1.0,2.0};if(!std::is_sorted(bins.begin(),bins.end())||std::any_of(bins.begin(),bins.end(),[](double value){return !std::isfinite(value)||value<=0;}))throw std::invalid_argument("diameter bins must be finite, positive and sorted");
 std::vector<double> class_lengths(bins.size()+1,0.0);for(const auto& edge:graph){total_measure_length+=edge.length_measure;if(edge.owner>=result.components.size())continue;const auto& component=result.components[edge.owner];if(component.touches_image_boundary||component.touches_roi_boundary||component.touches_abstained_area)continue;const double a_distance=edt[edge.first],b_distance=edt[edge.second];if(!std::isfinite(a_distance)||!std::isfinite(b_distance))continue;const double diameter=(2.0*std::sqrt(a_distance)+2.0*std::sqrt(b_distance))/2.0;weighted_diameter+=diameter*edge.length_measure;measurable_length+=edge.length_measure;min_diameter=std::min(min_diameter,diameter);max_diameter=std::max(max_diameter,diameter);const auto position=std::upper_bound(bins.begin(),bins.end(),diameter)-bins.begin();class_lengths[static_cast<std::size_t>(position)]+=edge.length_measure;}
 result.diameter_unit=area_per_pixel?"mm":"px";result.measurable_diameter_length_fraction=total_measure_length==0?0.0:measurable_length/total_measure_length;if(measurable_length>0){result.mean_diameter=weighted_diameter/measurable_length;result.min_diameter=min_diameter;result.max_diameter=max_diameter;double lower=0.0;for(std::size_t i=0;i<class_lengths.size();++i){DiameterClass item;item.lower_bound=lower;if(i<bins.size()){item.upper_bound=bins[i];lower=bins[i];}item.length=class_lengths[i];item.percent=100.0*item.length/measurable_length;result.diameter_classes.push_back(item);}}
 return result;
}

}
