// RIZOMA-IMG-EXP-003: deterministic, lossless pixel-class comparison.
// This component does not infer biological identity and does not train a model.
#include "rizoma.hpp"
#include <algorithm>
#include <stdexcept>
#include <deque>
#include <vector>
namespace rizoma {
namespace {
void valid(const Image& image,const Gray& a,const Gray& b,int border) {
 if(image.width<=0||image.height<=0||border<0)throw std::invalid_argument("Dimensoes/borda invalidas");
 const auto size=std::size_t(image.width)*image.height;
 if(image.pixels.size()!=size||a.pixels.size()!=size||b.pixels.size()!=size ||
    a.width!=image.width||a.height!=image.height||b.width!=image.width||b.height!=image.height)
    throw std::invalid_argument("Imagens de comparacao com formas diferentes");
}
Rgb blend(Rgb a,Rgb tint) {
 // alpha=0.76; quantitative results are written in binary class-map, not extracted from overlay
 return {static_cast<std::uint8_t>((24*int(a.r)+76*int(tint.r)+50)/100),
         static_cast<std::uint8_t>((24*int(a.g)+76*int(tint.g)+50)/100),
         static_cast<std::uint8_t>((24*int(a.b)+76*int(tint.b)+50)/100)};
}
}
Comparison compare_methods(const Image& image,const Gray& legacy,const Gray& adaptive,int review_border) {
 valid(image,legacy,adaptive,review_border);
 Comparison result{};
 result.visualization=image;
 result.class_map={image.width,image.height,std::vector<std::uint8_t>(image.pixels.size())};
 Gray union_mask{image.width,image.height,std::vector<std::uint8_t>(image.pixels.size())};
 const int w=image.width,h=image.height;
 for(std::size_t i=0;i<image.pixels.size();++i){
   const bool l=legacy.pixels[i]!=0, a=adaptive.pixels[i]!=0;
   if(l&&a){++result.counts.shared;result.class_map.pixels[i]=255;result.visualization.pixels[i]=blend(image.pixels[i],{16,225,183});}
   else if(l){++result.counts.legacy_only;result.class_map.pixels[i]=85;result.visualization.pixels[i]=blend(image.pixels[i],{255,159,29});}
   else if(a){++result.counts.adaptive_only;result.class_map.pixels[i]=170;result.visualization.pixels[i]=blend(image.pixels[i],{191,105,255});}
   else ++result.counts.neither;
   if(l||a)union_mask.pixels[i]=255;
 }
 // Connected components of the UNION; a single region may contain more than one method.
 std::vector<std::uint8_t> visited(union_mask.pixels.size(),0);
 std::deque<std::size_t> queue;
 for(int y=0;y<h;++y)for(int x=0;x<w;++x){
   const auto seed=std::size_t(y)*w+x;
   if(!union_mask.pixels[seed]||visited[seed])continue;
   ReviewRegion r{};r.id=int(result.regions.size()+1);r.x_min=r.x_max=x;r.y_min=r.y_max=y;
   queue.push_back(seed);visited[seed]=1;
   while(!queue.empty()){
     auto curr=queue.front();queue.pop_front();const int xx=int(curr%w),yy=int(curr/w);
     ++r.area;
     r.x_min=std::min(xx,r.x_min);r.x_max=std::max(xx,r.x_max);
     r.y_min=std::min(yy,r.y_min);r.y_max=std::max(yy,r.y_max);
     r.touches_border |= xx<review_border||xx>=w-review_border||yy<review_border||yy>=h-review_border;
     const auto cat=result.class_map.pixels[curr];
     if(cat==255)++r.shared;else if(cat==85)++r.legacy_only;else if(cat==170)++r.adaptive_only;
     for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
        if(!dx&&!dy)continue;
        int nx=xx+dx,ny=yy+dy;if(nx<0||ny<0||nx>=w||ny>=h)continue;
        auto ni=std::size_t(ny)*w+nx;
        if(!visited[ni]&&union_mask.pixels[ni]){visited[ni]=1;queue.push_back(ni);}
     }
   }
   result.regions.push_back(r);
 }
 // Deliberately do not rank by 'probability of root'; only size of disagreement.
 std::stable_sort(result.regions.begin(),result.regions.end(),[](const ReviewRegion& a,const ReviewRegion& b){
   auto da=a.legacy_only+a.adaptive_only,db=b.legacy_only+b.adaptive_only;
   return da==db?a.area>b.area:da>db;
 });
 for(std::size_t i=0;i<result.regions.size();++i)result.regions[i].id=int(i+1);
 return result;
}
}
