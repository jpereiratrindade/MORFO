#include "rizoma.hpp"
#include <iostream>
#include <filesystem>
#include <stdexcept>

int main(int argc,char** argv) {
 try {
  if(argc!=3)throw std::runtime_error("usage: test rgb.png expected-mask.png");
  const auto source=rizoma::read_rgb(argv[1]);
  const auto expected=rizoma::read_rgb(argv[2]);
  if(source.width!=850||source.height!=757||expected.width!=850||expected.height!=757)
   throw std::runtime_error("Fixture dimensions have changed");
  const auto gray=rizoma::grayscale(source);
  const auto th=rizoma::tophat(gray,8);
  const auto actual=rizoma::adaptive_detect(source,gray,th,rizoma::Params{});
  const auto summary=rizoma::summarize(actual.mask);
  if(summary.pixels!=3339||summary.components!=30)
   throw std::runtime_error("EXP-003 semantic regression: expected 3339 pixels / 30 components");
  std::size_t diffs=0;
  for(std::size_t i=0;i<actual.mask.pixels.size();++i){
   const auto& pixel=expected.pixels.at(i);
   if(actual.mask.pixels[i]!=pixel.r||pixel.r!=pixel.g||pixel.r!=pixel.b)++diffs;
  }
  if(diffs)throw std::runtime_error("EXP-003 mask differs at "+std::to_string(diffs)+" pixels");
  std::cout<<"MORFO ROOTS EXP-003 GOLDEN PASS: mask exact 850x757, 3339 pixels, 30 components\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
