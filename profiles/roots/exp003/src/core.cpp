#include "rizoma.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace rizoma {
namespace {
using U8=std::uint8_t;
Gray make_gray(int w,int h){return {w,h,std::vector<U8>(std::size_t(w)*h,0)};}
void assert_shape(const Gray& a,const Gray& b){if(a.width!=b.width||a.height!=b.height)throw std::runtime_error("Dimensões incompatíveis");}
struct Analysis {std::vector<int> labels;std::vector<Component> items;};
Analysis analyze(const Gray& binary){
 const int w=binary.width,h=binary.height;Analysis ans;
 ans.labels.assign(std::size_t(w)*h,-1);
 std::vector<std::size_t> queue;queue.reserve(4096);
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  const std::size_t at=std::size_t(y)*w+x;
  if(binary.pixels[at]==0||ans.labels[at]!=-1)continue;
  const int label=static_cast<int>(ans.items.size());queue.clear();queue.push_back(at);ans.labels[at]=label;
  Component c{};c.min_x=c.max_x=x;c.min_y=c.max_y=y;double sx=0,sy=0,sxx=0,syy=0,sxy=0;
  for(std::size_t front=0;front<queue.size();front++){
   auto j=queue[front];int px=int(j%w),py=int(j/w);++c.area;
   c.min_x=std::min(c.min_x,px);c.max_x=std::max(c.max_x,px);c.min_y=std::min(c.min_y,py);c.max_y=std::max(c.max_y,py);
   sx+=px;sy+=py;sxx+=double(px)*px;syy+=double(py)*py;sxy+=double(px)*py;
   for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
    if(dx==0&&dy==0)continue;
    const int nx=px+dx,ny=py+dy;
    if(nx<0||ny<0||nx>=w||ny>=h)continue;
    std::size_t ni=std::size_t(ny)*w+nx;
    if(binary.pixels[ni]&&ans.labels[ni]==-1){ans.labels[ni]=label;queue.push_back(ni);}
   }
  }
  const double inv=1./c.area;double a=std::max(0.,sxx*inv-(sx*inv)*(sx*inv));
  double b=std::max(0.,syy*inv-(sy*inv)*(sy*inv));double off=sxy*inv-(sx*inv)*(sy*inv);
  const double d=std::sqrt((a-b)*(a-b)+4*off*off);
  c.elongation=std::sqrt((a+b+d+0.1)/(std::max(0.,a+b-d)+0.1));
  ans.items.push_back(c);
 }
 return ans;
}
std::vector<std::pair<int,int>> ellipse_offsets(int radius){
 std::vector<std::pair<int,int>> offsets;
 if(radius==0)return {{0,0}};
 // Aproximação de elemento elíptico do OpenCV: cvRound(center*sqrt(1-(dy/r)^2)).
 for(int dy=-radius;dy<=radius;dy++){
  const double fract=1.-double(dy*dy)/(radius*radius);
  const int width=static_cast<int>(std::lround(radius*std::sqrt(std::max(0.,fract))));
  for(int dx=-width;dx<=width;dx++)offsets.emplace_back(dx,dy);
 }
 return offsets;
}
Gray morphology(const Gray& g,const std::vector<std::pair<int,int>>& offsets,bool dilate){
 Gray result=make_gray(g.width,g.height);const int w=g.width,h=g.height;
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  int val=dilate?0:255;
  for(const auto& [dx,dy]:offsets){const int nx=x+dx,ny=y+dy;
   if(nx<0||ny<0||nx>=w||ny>=h)continue;
   const int p=g.pixels[std::size_t(ny)*w+nx];
   if(dilate)val=std::max(val,p);else val=std::min(val,p);
  }
  result.pixels[std::size_t(y)*w+x]=static_cast<U8>(val);
 }
 return result;
}
Gray morph_open(const Gray& g,int rad){const auto se=ellipse_offsets(rad);return morphology(morphology(g,se,false),se,true);}
Gray median3(const Gray& img){
 Gray result=make_gray(img.width,img.height);auto w=img.width,h=img.height;
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  std::array<U8,9> values{};int n=0;
  for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
   auto xx=std::clamp(x+dx,0,w-1),yy=std::clamp(y+dy,0,h-1);
   values[n++]=img.pixels[std::size_t(yy)*w+xx];
  }
  std::nth_element(values.begin(),values.begin()+4,values.end());result.pixels[std::size_t(y)*w+x]=values[4];
 }
 return result;
}
// Distribuição robusta local por histograma, sem rótulos e sem memória persistente.
struct Tile{double median=0,mad=1;};
double percentile(const std::array<int,256>& hist,int target){
 int acc=0;for(int v=0;v<256;v++){acc+=hist[v];if(acc>target)return v;}return 255.;
}
std::vector<Tile> tile_matrix(const Gray& g,int cell,int context,int& nx,int& ny){
 nx=(g.width+cell-1)/cell;ny=(g.height+cell-1)/cell;
 std::vector<Tile> tiles(std::size_t(nx)*ny);
 for(int gy=0;gy<ny;gy++)for(int gx=0;gx<nx;gx++){
  const int cx=std::min(g.width-1,gx*cell+cell/2),cy=std::min(g.height-1,gy*cell+cell/2);
  int x0=std::max(0,cx-context/2),x1=std::min(g.width,cx+(context+1)/2);
  int y0=std::max(0,cy-context/2),y1=std::min(g.height,cy+(context+1)/2);
  std::array<int,256> hist{};int n=0;
  for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){hist[g.pixels[std::size_t(y)*g.width+x]]++;n++;}
  const auto med=static_cast<int>(percentile(hist,n/2));
  std::array<int,256> deviations{};
  for(int v=0;v<256;v++)deviations[std::abs(v-med)]+=hist[v];
  const double mad=percentile(deviations,n/2);
  tiles[std::size_t(gy)*nx+gx]={double(med),std::max(3.,1.4826*mad)};
 }
 return tiles;
}
Tile interpolate(const std::vector<Tile>& tiles,int nx,int ny,int cell,int x,int y){
 double ux=(double(x)+0.5)/cell-0.5,uy=(double(y)+0.5)/cell-0.5;
 ux=std::clamp(ux,0.,double(nx-1));uy=std::clamp(uy,0.,double(ny-1));
 int x0=int(std::floor(ux)),y0=int(std::floor(uy)),x1=std::min(nx-1,x0+1),y1=std::min(ny-1,y0+1);
 auto a=ux-x0,b=uy-y0;
 const Tile& t00=tiles[std::size_t(y0)*nx+x0];const Tile& t01=tiles[std::size_t(y0)*nx+x1];
 const Tile& t10=tiles[std::size_t(y1)*nx+x0];const Tile& t11=tiles[std::size_t(y1)*nx+x1];
 const auto blend=[&](double p,double q,double r,double s){return (1-a)*(1-b)*p+a*(1-b)*q+(1-a)*b*r+a*b*s;};
 return {blend(t00.median,t01.median,t10.median,t11.median),blend(t00.mad,t01.mad,t10.mad,t11.mad)};
}
}
Image negative(const Image& img){Image out=img;for(auto& c:out.pixels){c.r=U8(255-c.r);c.g=U8(255-c.g);c.b=U8(255-c.b);}return out;}
Image overlay(const Image& img,const Gray& mask){
 Image out=img;
 for(std::size_t i=0;i<out.pixels.size();i++)if(mask.pixels[i]){
  auto& c=out.pixels[i];c.r=U8(c.r*.35);c.g=U8(c.g*.35+255*.65);c.b=U8(c.b*.35+255*.65);
 }
 return out;
}
Gray grayscale(const Image& img){
 Gray out=make_gray(img.width,img.height);
 for(std::size_t i=0;i<img.pixels.size();i++){
  const auto& c=img.pixels[i];out.pixels[i]=U8(std::clamp(int(std::lround(.299*c.r+.587*c.g+.114*c.b)),0,255));
 }return out;
}
Gray tophat(const Gray& gray,int radius){
 const auto background=morph_open(gray,radius);Gray out=make_gray(gray.width,gray.height);
 for(std::size_t i=0;i<out.pixels.size();i++)out.pixels[i]=U8(std::max(0,int(gray.pixels[i])-int(background.pixels[i])));
 return out;
}
Gray legacy_score(const Image& img,const Gray& g,const Gray& th){
 assert_shape(g,th);Gray score=make_gray(img.width,img.height);
 for(std::size_t i=0;i<score.pixels.size();i++){
  const auto& c=img.pixels[i];int min=std::min({c.r,c.g,c.b});int max=std::max({c.r,c.g,c.b});
  score.pixels[i]=U8(std::clamp(int(g.pixels[i])+2*int(th.pixels[i])-(max-min)/2,0,255));
 }return score;
}
Gray legacy_mask(const Image& img,const Gray& g,const Gray& th){
 auto score=legacy_score(img,g,th);Gray mask=make_gray(img.width,img.height);
 for(std::size_t i=0;i<mask.pixels.size();i++)mask.pixels[i]=(score.pixels[i]>140||g.pixels[i]>108)?255:0;
 auto identified=analyze(mask);std::vector<bool> keep(identified.items.size(),false);
 for(std::size_t label=0;label<identified.items.size();label++){
  auto c=identified.items[label];int w=c.max_x-c.min_x+1,h=c.max_y-c.min_y+1;
  double aspect=std::max(double(w)/h,double(h)/w),fill=double(c.area)/(w*h);
  keep[label]=c.area>=4&&c.area<=2500&&(aspect>=1.5||c.area<80||fill<0.45);
 }
 for(std::size_t i=0;i<mask.pixels.size();i++){int label=identified.labels[i];mask.pixels[i]=label>=0&&keep[label]?255:0;}
 return morph_open(median3(mask),1);
}
Adaptive adaptive_detect(const Image& img,const Gray& g,const Gray& th,const Params& opts){
 if(opts.cell<8||opts.context<opts.cell||opts.border<0)throw std::runtime_error("Parâmetros de matriz inválidos");
 assert_shape(g,th);const int w=img.width,h=img.height;
 Adaptive result{make_gray(w,h),make_gray(w,h),0};int nx=0,ny=0;
 const auto tiles=tile_matrix(g,opts.cell,opts.context,nx,ny);
 Gray raw=make_gray(w,h);
 for(int y=0;y<h;y++)for(int x=0;x<w;x++){
  std::size_t i=std::size_t(y)*w+x;
  auto loc=interpolate(tiles,nx,ny,opts.cell,x,y);
  // MAD -> σ robusto local. Esta inferência NÃO equivale a probabilidade de raiz.
  const double excess=double(g.pixels[i])-loc.median;
  const double z=std::max(0.,excess/std::max(3.,loc.mad));
  result.zview.pixels[i]=U8(std::clamp(int(std::lround(z*40)),0,255));
  if(x<opts.border||y<opts.border||x>=w-opts.border||y>=h-opts.border){result.border_abstentions++;continue;}
  const auto& c=img.pixels[i];int color_range=std::max({c.r,c.g,c.b})-std::min({c.r,c.g,c.b});
  // Hipótese explícita para ESTA modalidade: filamentos CLAROS de alto contraste local.
  bool candidate=(z>=opts.seed_z&&th.pixels[i]>=opts.min_tophat&&excess>=7)
      ||(z>=4.0&&th.pixels[i]>=3&&excess>=10&&color_range<=24);
  if(candidate)raw.pixels[i]=255;
 }
 const auto labeled=analyze(raw);std::vector<bool> keep(labeled.items.size(),false);
 for(std::size_t n=0;n<labeled.items.size();n++){
  const auto& c=labeled.items[n];int cw=c.max_x-c.min_x+1,ch=c.max_y-c.min_y+1;
  const int major=std::max(cw,ch);double bbox_aspect=double(major)/std::min(cw,ch);
  keep[n]=c.area>=opts.min_component_area&&c.area<3000&&major>=7
          &&(bbox_aspect>=1.9||c.elongation>=3.2);
 }
 for(std::size_t i=0;i<result.mask.pixels.size();i++){
  int label=labeled.labels[i];result.mask.pixels[i]=(label>=0&&keep[label])?255:0;
 }
 return result;
}
std::vector<Component> components(const Gray& mask){return analyze(mask).items;}
Summary summarize(const Gray& mask){
 Summary out{};auto comps=components(mask);out.components=comps.size();
 for(std::size_t i=0;i<mask.pixels.size();i++)if(mask.pixels[i]){
  ++out.pixels;int row=int(i/mask.width);if(row>=mask.height-10)++out.bottom10;
 }
 for(auto c:comps)out.largest.push_back(c.area);
 std::sort(out.largest.begin(),out.largest.end(),std::greater{});if(out.largest.size()>10)out.largest.resize(10);
 return out;
}
std::size_t overlap(const Gray& a,const Gray& b){assert_shape(a,b);std::size_t count=0;for(std::size_t i=0;i<a.pixels.size();i++)if(a.pixels[i]&&b.pixels[i])count++;return count;}
}
