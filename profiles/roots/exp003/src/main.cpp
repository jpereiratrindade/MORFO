// RIZOMA-IMG-EXP-003: comparador de evidencias, conservando os detectores do EXP-002.
#include "rizoma.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rizoma;
namespace {
std::string escape_json(const std::string& s){std::string o;for(unsigned char c:s){if(c=='"'||c=='\\')o+='\\';if(c=='\n'){o+="\\n";continue;}if(c<32){o+='?';continue;}o+=char(c);}return o;}
std::string fixed(double n,int places=4){std::ostringstream os;os<<std::fixed<<std::setprecision(places)<<n;return os.str();}
void usage(){std::cout<<"Uso: rizoma-img-exp --input monolito.tiff --output pasta [--seed-z 2.4] [--border 10] [--cell 32] [--context 64]\n";}
}
int main(int argc,char** argv){
 try {
  fs::path input,output; Params params;
  for(int i=1;i<argc;i++){
   std::string key=argv[i];if(key=="--help"||key=="-h"){usage();return 0;}
   if(i+1>=argc)throw std::runtime_error("Argumento sem valor: "+key);
   std::string val=argv[++i];
   if(key=="--input")input=val;else if(key=="--output")output=val;
   else if(key=="--seed-z")params.seed_z=std::stod(val);
   else if(key=="--border")params.border=std::stoi(val);
   else if(key=="--cell")params.cell=std::stoi(val);
   else if(key=="--context")params.context=std::stoi(val);
   else throw std::runtime_error("Opção desconhecida: "+key);
  }
  if(input.empty()||output.empty()){usage();return 2;}
  if(!std::isfinite(params.seed_z)||params.seed_z<=0)throw std::runtime_error("seed-z inválido");
  if(fs::weakly_canonical(input)==fs::weakly_canonical(output))throw std::runtime_error("Saída não pode ser o arquivo original");
  fs::create_directories(output);
  const auto started=std::chrono::steady_clock::now();
  const auto img=read_rgb(input);auto gray=grayscale(img);
  const auto th=tophat(gray,params.top_hat_radius);
  const auto before=legacy_mask(img,gray,th);
  const auto after=adaptive_detect(img,gray,th,params);
  const auto old=summarize(before),now=summarize(after.mask);
  const auto shared=overlap(before,after.mask);
  const auto disagreement=compare_methods(img,before,after.mask,params.border);
  write_png(output/"01_original_visualizacao_rgb.png",img);
  write_png(output/"02_negativo_exato.png",negative(img));
  write_png(output/"03_tophat.png",th);
  write_png(output/"04_heuristica_anterior.png",before);
  write_png(output/"05_desvio_matriz_pontuacao.png",after.zview);
  write_png(output/"06_candidatos_adaptativos.png",after.mask);
  write_png(output/"07_sobreposicao_adaptativa.png",overlay(img,after.mask));
  write_png(output/"08_mapa_concordancia_rgb.png",disagreement.visualization);
  write_png(output/"09_classes_comparacao.png",disagreement.class_map);
  auto secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  const std::size_t total=img.pixels.size();
  std::ofstream report(output/"report.json");if(!report)throw std::runtime_error("Impossível gravar relatório");
  report<<"{\n  \"experiment\": \"RIZOMA-IMG-EXP-002\",\n"
        <<"  \"version\": \"0.1.0\",\n"
        <<"  \"mode\": \"no-pretrained-weights,per-image-robust-matrix\",\n"
        <<"  \"input_file\": \""<<escape_json(input.filename().string())<<"\",\n"
        <<"  \"input_sha256\": \""<<sha256_file(input)<<"\",\n"
        <<"  \"image_width\": "<<img.width<<",\n  \"image_height\": "<<img.height<<",\n"
        <<"  \"parameters\": {\"cell\": "<<params.cell<<", \"context\": "<<params.context<<", \"border\": "<<params.border<<", \"top_hat_radius\": "<<params.top_hat_radius<<", \"seed_z\": "<<fixed(params.seed_z)<<", \"min_tophat\": "<<fixed(params.min_tophat)<<"},\n"
        <<"  \"legacy\": {\"candidate_pixels\": "<<old.pixels<<", \"components\": "<<old.components<<", \"bottom10_pixels\": "<<old.bottom10<<", \"candidate_fraction\": "<<fixed(double(old.pixels)/total,8)<<"},\n"
        <<"  \"adaptive\": {\"candidate_pixels\": "<<now.pixels<<", \"components\": "<<now.components<<", \"bottom10_pixels\": "<<now.bottom10<<", \"candidate_fraction\": "<<fixed(double(now.pixels)/total,8)<<", \"border_abstentions\": "<<after.border_abstentions<<"},\n"
        <<"  \"comparison\": {\"intersection_pixels\": "<<shared<<", \"union_pixels\": "<<(old.pixels+now.pixels-shared)<<", \"jaccard_masks\": "<<fixed((old.pixels+now.pixels-shared)?double(shared)/(old.pixels+now.pixels-shared):1.,8)<<"},\n"
        <<"  \"judgment\": {\"score_semantics\": \"robust-deviation-not-probability\", \"choice\": \"ANOMALY_CANDIDATE\", \"unknown\": \"BIOLOGICAL_IDENTITY_UNVERIFIED\", \"noul\": \"ABSTAIN_ON_BORDER\", \"engine\": \"local-experimental-adapter-not-real-JEV\"},\n"
        <<"  \"scientific_status\": \"NOT_VALIDATED\",\n"
        <<"  \"duration_seconds\": "<<fixed(secs,3)<<"\n}\n";
  report.close();
  // CSV is a queue of regions to REVIEW, not a table of biological roots.
  std::ofstream regions(output/"10_regioes_para_revisao.csv");
  if(!regions)throw std::runtime_error("Falha ao criar CSV de revisao");
  regions<<"region_id,x_min,y_min,x_max,y_max,union_pixels,shared_pixels,legacy_only_pixels,adaptive_only_pixels,touches_border,biological_identity\n";
  for(const auto& reg:disagreement.regions)
    regions<<reg.id<<','<<reg.x_min<<','<<reg.y_min<<','<<reg.x_max<<','<<reg.y_max<<','
      <<reg.area<<','<<reg.shared<<','<<reg.legacy_only<<','<<reg.adaptive_only<<','
      <<(reg.touches_border?"true":"false")<<",UNKNOWN\n";
  if(!regions)throw std::runtime_error("Falha de escrita CSV");
  regions.close();
  std::ofstream evidence(output/"11_evidencias_para_julgamento.json");
  if(!evidence)throw std::runtime_error("Falha ao criar relatorio de evidencias");
  const auto& cnt=disagreement.counts;
  evidence<<"{\n  \"experiment\": \"RIZOMA-IMG-EXP-003\",\n"
    <<"  \"schema\": \"rizoma.comparison.evidence.v0.1.0\",\n"
    <<"  \"input_sha256\": \""<<sha256_file(input)<<"\",\n"
    <<"  \"semantic_note\": \"Differences between algorithms are not root probabilities\",\n"
    <<"  \"pixel_classes\": {\"neither\": "<<cnt.neither<<", \"shared\": "<<cnt.shared
    <<", \"legacy_only\": "<<cnt.legacy_only<<", \"adaptive_only\": "<<cnt.adaptive_only<<"},\n"
    <<"  \"jaccard\": "<<fixed((cnt.shared+cnt.legacy_only+cnt.adaptive_only)?
                         double(cnt.shared)/(cnt.shared+cnt.legacy_only+cnt.adaptive_only):1.,8)<<",\n"
    <<"  \"regions_for_review\": "<<disagreement.regions.size()<<",\n"
    <<"  \"proposed_epistemic_status\": {\"observation\": \"OBSERVED\", \"mask\": \"DERIVED\", \"root_identity\": \"UNKNOWN\"},\n"
    <<"  \"proposed_jev_response\": {\"type\": \"Noul\", \"reason\": \"InsufficientEvidence\", \"rationale\": \"No annotated biological ground truth supplied\"},\n"
    <<"  \"integration_status\": \"evidence-contract-only; JEV/ENTE engines not invoked\"\n}\n";
  if(!evidence)throw std::runtime_error("Falha de escrita JSON");
  evidence.close();
  std::cout<<"RIZOMA-IMG-EXP-003 PASS: "<<img.width<<"x"<<img.height<<"\n"
           <<"Heurística: "<<old.pixels<<" pixels, "<<old.components<<" componentes, borda inferior "<<old.bottom10<<"\n"
           <<"Matriz adaptativa: "<<now.pixels<<" pixels, "<<now.components<<" componentes, borda inferior "<<now.bottom10<<"\n"
           <<"Sobreposição: "<<shared<<" pixels | tempo "<<fixed(secs,2)<<" s\n"
           <<"Comparacao: comum="<<cnt.shared<<" anterior="<<cnt.legacy_only<<" adaptativo="<<cnt.adaptive_only<<" regioes="<<disagreement.regions.size()<<"\n"
           <<"Interpretação: ANOMALY_CANDIDATE / identidade biológica UNKNOWN.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<"ERRO: "<<e.what()<<"\n";return 1;}
}
