#include "rizoma.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
using namespace rizoma;
namespace {
Image scene(int width,int height){Image x{width,height,{}};x.pixels.assign(std::size_t(width)*height,{50,48,45});return x;}
void line(Image& x,int x0,int x1,int y, Rgb rgb={220,220,215}){
 for(int i=x0;i<x1;i++)for(int dy=-1;dy<=1;dy++)x.pixels[std::size_t(y+dy)*x.width+i]=rgb;
}
}
int main(){
 {Image im=scene(3,3);im.pixels[0]={0,127,255};auto inverse=negative(im);assert(inverse.pixels[0].r==255&&inverse.pixels[0].g==128&&inverse.pixels[0].b==0);assert(negative(inverse).pixels==im.pixels);}
 {
  Image im=scene(128,96);line(im,28,90,48);for(int x=0;x<128;x++)im.pixels[x]={250,250,250};
  Params p;auto gray=grayscale(im);auto th=tophat(gray,p.top_hat_radius);
  const auto result=adaptive_detect(im,gray,th,p);
  const auto summary=summarize(result.mask);
  assert(summary.pixels>0);assert(summary.components>=1);
  int recognized=0;for(int x=28;x<90;x++)if(result.mask.pixels[std::size_t(48)*128+x])recognized++;
  assert(recognized>=35); // tendência central clara e linear, sem corpus rotulado
  for(int x=0;x<128;x++)assert(result.mask.pixels[x]==0); // borda é um caso de abstenção explícita
  assert(result.border_abstentions>0);
  assert(overlap(result.mask,result.mask)==summary.pixels);
 }
 {Image im=scene(64,64);auto g=grayscale(im);auto th=tophat(g,8);auto result=adaptive_detect(im,g,th,Params{});assert(summarize(result.mask).pixels==0);}
 {
  Image im=scene(4,3);Gray left{4,3,std::vector<std::uint8_t>(12,0)};
  Gray right=left;
  left.pixels[0]=255;left.pixels[1]=255;
  right.pixels[1]=255;right.pixels[2]=255;
  const auto comparison=compare_methods(im,left,right,1);
  assert(comparison.counts.neither==9 && comparison.counts.legacy_only==1);
  assert(comparison.counts.adaptive_only==1 && comparison.counts.shared==1);
  assert(comparison.counts.total()==12);
  assert(comparison.class_map.pixels[0]==85 && comparison.class_map.pixels[1]==255);
  assert(comparison.class_map.pixels[2]==170 && comparison.class_map.pixels[3]==0);
  assert(comparison.regions.size()==1 && comparison.regions[0].area==3);
  assert(comparison.regions[0].touches_border);
  auto no_method=compare_methods(im,Gray{4,3,std::vector<std::uint8_t>(12,0)},Gray{4,3,std::vector<std::uint8_t>(12,0)});
  assert(no_method.regions.empty() && no_method.counts.neither==12);
  bool threw=false;
  try { auto mismatch=right;mismatch.width=3; (void)compare_methods(im,left,mismatch); }
  catch(const std::invalid_argument&) {threw=true;}
  assert(threw);
 }
 std::cout<<"PASS: negativo reversível, matriz uniforme, filamento detectado, borda rejeitada, autointerseção\n";
}
