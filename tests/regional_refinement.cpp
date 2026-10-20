#include "regional_refinement.hpp"
#include <cassert>
#include <iostream>
using namespace morfo::refinement;
int main(){
 Mask base{12,10,std::vector<std::uint8_t>(120)};for(std::size_t i=0;i<base.pixels.size();i+=7)base.pixels[i]=255;
 auto a=rectangle("a",12,10,2,2,4,3);Mask proposed=base;for(auto& p:proposed.pixels)p=p?0:255;
 auto changed=merge(base,proposed,a.mask);
 for(std::size_t i=0;i<base.pixels.size();++i)assert(a.mask.pixels[i]||changed.pixels[i]==base.pixels[i]);
 auto b=rectangle("b",12,10,4,3,5,4);auto excluded=apply(changed,b.mask,Kind::Exclude);
 for(int y=3;y<7;++y)for(int x=4;x<9;++x)assert(excluded.pixels[std::size_t(y)*12+x]==0);
 for(std::size_t i=0;i<changed.pixels.size();++i)if(!b.mask.pixels[i])assert(excluded.pixels[i]==changed.pixels[i]);
 Timeline history(base);history.commit({"r1","","a","{\"score_threshold\":130}","t1",Kind::Reprocess,changed});history.commit({"r2","","b","{}","t2",Kind::Exclude,excluded});assert(history.current().id=="r2");assert(history.undo()&&history.current().id=="r1");assert(history.redo()&&history.current().id=="r2");
 auto poly=polygon("p",12,10,{{1,1},{10,1},{5,8}});assert(poly.mask.pixels[3*12+5]);auto stroke=brush("s",12,10,{{1,1},{10,8}},3);assert(stroke.mask.pixels[1*12+1]);
 rizoma::ReferenceParams params;rizoma::validate(params);bool rejected=false;try{params.max_component_area=2;rizoma::validate(params);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 rizoma::Image image{12,10,std::vector<rizoma::Rgb>(120)};for(std::size_t i=0;i<image.pixels.size();++i)image.pixels[i]={std::uint8_t((i*17)%256),std::uint8_t((i*29)%256),std::uint8_t((i*7)%256)};const auto original=image;const auto gray=rizoma::grayscale(image),top=rizoma::tophat(gray,8);assert(rizoma::reference_mask(image,{}) .pixels==rizoma::legacy_mask(image,gray,top).pixels);assert(image.pixels==original.pixels);
 std::cout<<"regional refinement PASS\n";
}
