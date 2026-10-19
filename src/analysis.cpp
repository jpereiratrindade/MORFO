#include "analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace rizoma::analysis {

Result detect(std::uint64_t width,std::uint64_t height,const std::vector<unsigned char>& rgba){
 if(!width||!height||width>static_cast<std::uint64_t>(SIZE_MAX)/height||width*height>SIZE_MAX/4||rgba.size()!=width*height*4)
  throw std::runtime_error("Invalid raster for experimental analysis");
 const std::size_t count=static_cast<std::size_t>(width*height);
 std::vector<unsigned char> gray(count),global(count),local(count),visited(count);
 double sum=0,squares=0;
 for(std::size_t i=0;i<count;++i){
  const auto r=rgba[i*4],g=rgba[i*4+1],b=rgba[i*4+2];
  const auto value=static_cast<unsigned char>(std::clamp(std::lround(.299*r+.587*g+.114*b),0L,255L));
  gray[i]=value;sum+=value;squares+=double(value)*value;
 }
 Result result;result.global_mean=sum/count;
 result.global_stddev=std::sqrt(std::max(0.,squares/count-result.global_mean*result.global_mean));
 const double global_threshold=result.global_mean+std::max(12.,1.20*result.global_stddev);
 constexpr std::uint64_t cell=32;
 const auto cells_x=(width+cell-1)/cell,cells_y=(height+cell-1)/cell;
 std::vector<double> means(static_cast<std::size_t>(cells_x*cells_y));
 std::vector<double> deviations(means.size());
 for(std::uint64_t cy=0;cy<cells_y;++cy)for(std::uint64_t cx=0;cx<cells_x;++cx){
  const auto x0=cx*cell,x1=std::min(width,x0+cell),y0=cy*cell,y1=std::min(height,y0+cell);
  double s=0,sq=0,n=double((x1-x0)*(y1-y0));
  for(auto y=y0;y<y1;++y)for(auto x=x0;x<x1;++x){double v=gray[static_cast<std::size_t>(y*width+x)];s+=v;sq+=v*v;}
  const auto at=static_cast<std::size_t>(cy*cells_x+cx);means[at]=s/n;deviations[at]=std::sqrt(std::max(9.,sq/n-means[at]*means[at]));
 }
 for(std::uint64_t y=0;y<height;++y)for(std::uint64_t x=0;x<width;++x){
  const auto i=static_cast<std::size_t>(y*width+x),base=i*4;
  const int hi=std::max({int(rgba[base]),int(rgba[base+1]),int(rgba[base+2])});
  const int lo=std::min({int(rgba[base]),int(rgba[base+1]),int(rgba[base+2])});
  if(gray[i]>=global_threshold&&hi-lo<=48)global[i]=1;
  const auto cell_at=static_cast<std::size_t>((y/cell)*cells_x+x/cell);
  if(gray[i]>=means[cell_at]+std::max(9.,1.45*deviations[cell_at])&&hi-lo<=48)local[i]=1;
 }
 std::deque<std::size_t> queue;
 for(std::uint64_t y=0;y<height;++y)for(std::uint64_t x=0;x<width;++x){
  const auto seed=static_cast<std::size_t>(y*width+x);
  if(visited[seed]||(!global[seed]&&!local[seed]))continue;
  Region region{x,y,1,1,0,0,0,0,false};
  auto max_x=x,max_y=y;visited[seed]=1;queue.push_back(seed);
  while(!queue.empty()){
   const auto at=queue.front();queue.pop_front();const auto px=at%width,py=at/width;
   ++region.area;region.global_pixels+=global[at];region.local_pixels+=local[at];region.shared_pixels+=global[at]&&local[at];
   region.x=std::min(region.x,px);region.y=std::min(region.y,py);max_x=std::max(max_x,px);max_y=std::max(max_y,py);
   region.touches_border|=px==0||py==0||px+1==width||py+1==height;
   for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
    if(!dx&&!dy)continue;
    const auto nx=static_cast<std::int64_t>(px)+dx,ny=static_cast<std::int64_t>(py)+dy;
    if(nx<0||ny<0||nx>=static_cast<std::int64_t>(width)||ny>=static_cast<std::int64_t>(height))continue;
    const auto next=static_cast<std::size_t>(ny*static_cast<std::int64_t>(width)+nx);
    if(!visited[next]&&(global[next]||local[next])){visited[next]=1;queue.push_back(next);}
   }
  }
  region.width=max_x-region.x+1;region.height=max_y-region.y+1;
  const double aspect=double(std::max(region.width,region.height))/std::max<std::uint64_t>(1,std::min(region.width,region.height));
  if(region.area>=4&&(aspect>=1.7||region.area<=24))result.regions.push_back(region);
 }
 std::stable_sort(result.regions.begin(),result.regions.end(),[](const Region& a,const Region& b){
  if(a.shared_pixels!=b.shared_pixels)return a.shared_pixels>b.shared_pixels;
  return a.area>b.area;
 });
 return result;
}

} // namespace rizoma::analysis
