#include "morfo_pre.hpp"
#include <cassert>
#include <set>
#include <vector>
int main(){constexpr std::size_t w=64,h=64;std::vector<unsigned char> rgba(w*h*4,255);for(std::size_t y=0;y<h;++y)for(std::size_t x=0;x<w;++x){auto i=(y*w+x)*4;if(x<32){rgba[i]=20;rgba[i+1]=30;rgba[i+2]=40;}else{rgba[i]=220;rgba[i+1]=180;rgba[i+2]=30;}rgba[i+3]=255;}const auto a=morfo::pre::perceive(w,h,rgba,{16,2,20}),b=morfo::pre::perceive(w,h,rgba,{16,2,20});assert(a.cells.size()==16&&a.prototypes.size()==2);std::set<std::size_t> groups;for(std::size_t i=0;i<a.cells.size();++i){groups.insert(a.cells[i].cluster);assert(a.cells[i].cluster==b.cells[i].cluster);assert(a.cells[i].novelty>=0);}assert(groups.size()==2);assert(a.cells.front().cluster!=a.cells.back().cluster);}
