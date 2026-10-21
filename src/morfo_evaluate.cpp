#include "morfo_evaluate.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

namespace morfo::evaluate {
namespace {
void same_nonempty(std::size_t a,std::size_t b){
 if(a==0||a!=b)throw std::invalid_argument("prediction and reference must be non-empty and equally sized");
}
double ratio(double numerator,double denominator){return denominator==0.0?0.0:numerator/denominator;}
double choose2(std::uint64_t n){return n<2?0.0:static_cast<double>(n)*static_cast<double>(n-1)/2.0;}
}

BinaryMetrics binary(std::span<const std::uint8_t> prediction,std::span<const std::uint8_t> reference){
 same_nonempty(prediction.size(),reference.size()); BinaryMetrics m;
 for(std::size_t i=0;i<prediction.size();++i){
  if(prediction[i]>1||reference[i]>1)throw std::invalid_argument("binary masks accept only 0 and 1");
  if(prediction[i]&&reference[i])++m.true_positive;
  else if(!prediction[i]&&!reference[i])++m.true_negative;
  else if(prediction[i])++m.false_positive;
  else ++m.false_negative;
 }
 const double tp=m.true_positive,tn=m.true_negative,fp=m.false_positive,fn=m.false_negative;
 m.precision=ratio(tp,tp+fp);m.recall=ratio(tp,tp+fn);
 m.f1=ratio(2*tp,2*tp+fp+fn);m.dice=m.f1;m.iou=ratio(tp,tp+fp+fn);
 m.accuracy=ratio(tp+tn,tp+tn+fp+fn);return m;
}

MeasurementMetrics measurement(std::span<const double> prediction,std::span<const double> reference){
 same_nonempty(prediction.size(),reference.size()); MeasurementMetrics m{};m.count=prediction.size();
 double absolute=0,squared=0,signed_error=0;
 for(std::size_t i=0;i<prediction.size();++i){
  if(!std::isfinite(prediction[i])||!std::isfinite(reference[i]))throw std::invalid_argument("measurements must be finite");
  const double e=prediction[i]-reference[i];absolute+=std::abs(e);squared+=e*e;signed_error+=e;
 }
 m.mae=absolute/m.count;m.rmse=std::sqrt(squared/m.count);m.bias=signed_error/m.count;return m;
}

double adjusted_rand_index(std::span<const std::int64_t> discovered,std::span<const std::int64_t> reference){
 same_nonempty(discovered.size(),reference.size());
 std::map<std::pair<std::int64_t,std::int64_t>,std::uint64_t> cells;
 std::map<std::int64_t,std::uint64_t> rows,cols;
 for(std::size_t i=0;i<discovered.size();++i){++cells[{discovered[i],reference[i]}];++rows[discovered[i]];++cols[reference[i]];}
 double index=0,row_pairs=0,col_pairs=0;
 for(const auto& [_,n]:cells)index+=choose2(n);
 for(const auto& [_,n]:rows)row_pairs+=choose2(n);
 for(const auto& [_,n]:cols)col_pairs+=choose2(n);
 const double all=choose2(discovered.size());if(all==0)return 1.0;
 const double expected=row_pairs*col_pairs/all,maximum=(row_pairs+col_pairs)/2.0,denominator=maximum-expected;
 return denominator==0.0?1.0:(index-expected)/denominator;
}

double silhouette(std::span<const double> points,std::size_t dimensions,std::span<const std::int64_t> labels){
 if(dimensions==0||labels.empty()||points.size()!=labels.size()*dimensions)throw std::invalid_argument("invalid silhouette matrix shape");
 if(!std::all_of(points.begin(),points.end(),[](double v){return std::isfinite(v);}))throw std::invalid_argument("points must be finite");
 const std::set<std::int64_t> clusters(labels.begin(),labels.end());if(clusters.size()<2)throw std::invalid_argument("silhouette requires at least two clusters");
 auto distance=[&](std::size_t a,std::size_t b){double sum=0;for(std::size_t d=0;d<dimensions;++d){const double x=points[a*dimensions+d]-points[b*dimensions+d];sum+=x*x;}return std::sqrt(sum);};
 double total=0;
 for(std::size_t i=0;i<labels.size();++i){
  std::map<std::int64_t,std::pair<double,std::size_t>> distances;
  for(std::size_t j=0;j<labels.size();++j)if(i!=j){auto& value=distances[labels[j]];value.first+=distance(i,j);++value.second;}
  auto own=distances.find(labels[i]);if(own==distances.end()){continue;}
  const double a=own->second.first/own->second.second;double b=INFINITY;
  for(const auto& [label,value]:distances)if(label!=labels[i])b=std::min(b,value.first/value.second);
  total+=(b-a)/std::max(a,b);
 }
 return total/labels.size();
}

Interval bootstrap_mean_95(std::span<const double> scores,std::size_t resamples,std::uint64_t seed){
 if(scores.empty()||resamples<100)throw std::invalid_argument("bootstrap requires scores and at least 100 resamples");
 if(!std::all_of(scores.begin(),scores.end(),[](double v){return std::isfinite(v);}))throw std::invalid_argument("scores must be finite");
 Interval result{};result.samples=resamples;result.estimate=std::accumulate(scores.begin(),scores.end(),0.0)/scores.size();
 std::mt19937_64 engine(seed);std::uniform_int_distribution<std::size_t> pick(0,scores.size()-1);std::vector<double> means(resamples);
 for(double& mean:means){double sum=0;for(std::size_t i=0;i<scores.size();++i)sum+=scores[pick(engine)];mean=sum/scores.size();}
 std::sort(means.begin(),means.end());auto quantile=[&](double p){const double x=p*(means.size()-1);const auto lo=static_cast<std::size_t>(x),hi=std::min(lo+1,means.size()-1);return means[lo]+(means[hi]-means[lo])*(x-lo);};
 result.lower=quantile(.025);result.upper=quantile(.975);return result;
}
} // namespace morfo::evaluate
