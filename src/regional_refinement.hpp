#pragma once
#include "rizoma.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace morfo::refinement {
using Mask=rizoma::Gray;
enum class Kind { Reprocess, Include, Exclude };
struct Selection {
 std::string id,type,geometry_json;
 Mask mask;
};
Selection rectangle(std::string id,int width,int height,int x,int y,int selection_width,int selection_height);
Selection polygon(std::string id,int width,int height,const std::vector<std::pair<double,double>>& points);
Selection brush(std::string id,int width,int height,const std::vector<std::pair<double,double>>& points,double diameter);
Mask merge(const Mask& current,const Mask& proposed,const Mask& region);
Mask apply(const Mask& current,const Mask& region,Kind kind,const Mask* proposed=nullptr);
struct Revision {std::string id,previous_id,selection_id,parameters_json,created_at;Kind kind=Kind::Reprocess;Mask mask;};
class Timeline {
public:
 explicit Timeline(Mask initial);
 const Revision& commit(Revision revision);
 bool undo(); bool redo();
 const Revision& current() const; const std::vector<Revision>& revisions() const{return items_;}
private: std::vector<Revision> items_;std::size_t cursor_=0;
};
}
