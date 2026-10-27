#include "morfo_evaluate.hpp"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

static bool near(double a,double b,double e=1e-9){return std::abs(a-b)<e;}
int main(){
 using namespace morfo::evaluate;
 const std::vector<std::uint8_t> prediction{1,1,0,0,1,0},reference{1,0,1,0,1,0};
 const auto b=binary(prediction,reference);assert(b.true_positive==2&&b.true_negative==2&&b.false_positive==1&&b.false_negative==1);assert(near(b.dice,2.0/3)&&near(b.iou,.5));
 assert(b.precision_applicable&&b.recall_applicable&&b.overlap_applicable);
 const auto empty_masks=binary(std::vector<std::uint8_t>{0,0},std::vector<std::uint8_t>{0,0});
 assert(!empty_masks.precision_applicable&&!empty_masks.recall_applicable&&!empty_masks.overlap_applicable);
 assert(near(empty_masks.accuracy,1)&&near(empty_masks.dice,0));
 const auto false_positive_only=binary(std::vector<std::uint8_t>{1},std::vector<std::uint8_t>{0});
 assert(false_positive_only.precision_applicable&&!false_positive_only.recall_applicable&&false_positive_only.overlap_applicable);
 const std::vector<double> measured{2,4,8},truth{1,5,6};const auto m=measurement(measured,truth);assert(near(m.mae,4.0/3)&&near(m.rmse,std::sqrt(2.0))&&near(m.bias,2.0/3));
 const std::vector<std::int64_t> groups{0,0,1,1,2,2},renamed{8,8,4,4,9,9},mixed{0,1,0,1,0,1};assert(near(adjusted_rand_index(groups,renamed),1));assert(adjusted_rand_index(groups,mixed)<0);
 const std::vector<double> points{0,0,0,1,10,10,10,11};const std::vector<std::int64_t> labels{0,0,1,1};assert(silhouette(points,2,labels)>.9);
 const std::vector<double> scores{.7,.8,.9,1};const auto ci=bootstrap_mean_95(scores,1000,42);assert(near(ci.estimate,.85)&&ci.lower<=ci.estimate&&ci.upper>=ci.estimate);
 bool rejected=false;try{binary(std::vector<std::uint8_t>{2},std::vector<std::uint8_t>{1});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
}
