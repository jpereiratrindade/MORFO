// MORFO v0.1.0: image-first, local-only acquisition and reproducible previews.
// Copyright (C) 2026 José Pedro Trindade
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// Previews are for quality inspection, NOT scientifically validated measurements.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <png.h>
#include <jpeglib.h>
#include <tiffio.h>
#include "database.hpp"
#include "analysis.hpp"
#include "tiles.hpp"
#include "learning.hpp"
#include "morfo_profiles.hpp"
#include "morfo_root_metrics.hpp"
#include "regional_refinement.hpp"
#include "auth_smtp.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <setjmp.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace fs=std::filesystem;
namespace asio=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
using tcp=asio::ip::tcp;
constexpr std::uint64_t MAX_UPLOAD=8ULL*1024*1024*1024;
constexpr std::size_t MAX_CONTROL_BODY=1024*1024;
constexpr std::uint64_t MAX_PIXELS=12'000'000;
constexpr int THUMB_MAX=1280;
constexpr int TILE_SIZE=256;
constexpr const char* PIPELINE="rizoma-exp004-progressive-0.4.0";
struct Failure : std::runtime_error {using std::runtime_error::runtime_error;};
struct Raster {int w=0,h=0;std::vector<unsigned char> rgba;};
struct App {
    fs::path root, web;
    std::string host = "0.0.0.0";
    unsigned short port = 8091;
    std::atomic<int> active{0};
    mutable morfo::auth::AuthSmtpService auth_service{morfo::auth::AuthSmtpConfig::from_environment()};
};
static std::string utc_offset(int seconds){auto t=std::time(nullptr)+seconds; std::tm v{};gmtime_r(&t,&v);char buf[32];std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ",&v);return buf;}
[[maybe_unused]] static std::string utc(){return utc_offset(0);}
static std::string esc(std::string_view s){std::string out;out.reserve(s.size()+8);for(unsigned char c:s){switch(c){case '"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<32){char b[7];std::snprintf(b,sizeof(b),"\\u%04x",c);out+=b;}else out+=char(c);}}return out;}
static std::string q(std::string_view s){return "\""+esc(s)+"\"";}
static std::string hex(const unsigned char* ptr,std::size_t n){static const char h[]="0123456789abcdef";std::string out;out.reserve(n*2);for(std::size_t i=0;i<n;++i){out+=h[ptr[i]>>4];out+=h[ptr[i]&15];}return out;}
static std::string hash256_file(const fs::path& path){
 std::ifstream input(path,std::ios::binary);if(!input)throw Failure("could not hash staged file");
 std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
 if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1)throw Failure("SHA-256 unavailable");
 std::array<char,65536> block{};while(input){input.read(block.data(),block.size());auto count=input.gcount();if(count>0&&EVP_DigestUpdate(context.get(),block.data(),static_cast<std::size_t>(count))!=1)throw Failure("SHA-256 failed");}
 unsigned char digest[EVP_MAX_MD_SIZE];unsigned int length=0;if(EVP_DigestFinal_ex(context.get(),digest,&length)!=1)throw Failure("SHA-256 failed");return hex(digest,length);
}
static std::string hash256_text(std::string_view value){unsigned char digest[EVP_MAX_MD_SIZE];unsigned int length=0;std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1||EVP_DigestUpdate(context.get(),value.data(),value.size())!=1||EVP_DigestFinal_ex(context.get(),digest,&length)!=1)throw Failure("SHA-256 failed");return hex(digest,length);}
static std::uint32_t zip_crc32(std::string_view data){std::uint32_t crc=0xffffffffU;for(unsigned char byte:data){crc^=byte;for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320U:0U);}return crc^0xffffffffU;}
static void zip_u16(std::string& out,std::uint16_t value){out.push_back(static_cast<char>(value));out.push_back(static_cast<char>(value>>8));}
static void zip_u32(std::string& out,std::uint32_t value){for(int shift=0;shift<32;shift+=8)out.push_back(static_cast<char>(value>>shift));}
struct ZipEntry{std::string name,data;std::uint32_t crc=0,offset=0;};
static std::string zip_store(std::vector<ZipEntry> entries){
 std::string out;for(auto& entry:entries){if(entry.name.size()>65535||entry.data.size()>0xffffffffULL||out.size()>0xffffffffULL)throw Failure("Refinement export exceeds ZIP32 limits");entry.crc=zip_crc32(entry.data);entry.offset=static_cast<std::uint32_t>(out.size());zip_u32(out,0x04034b50);zip_u16(out,20);zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u32(out,entry.crc);zip_u32(out,static_cast<std::uint32_t>(entry.data.size()));zip_u32(out,static_cast<std::uint32_t>(entry.data.size()));zip_u16(out,static_cast<std::uint16_t>(entry.name.size()));zip_u16(out,0);out+=entry.name;out+=entry.data;}
 if(entries.size()>65535||out.size()>0xffffffffULL)throw Failure("Refinement export exceeds ZIP32 limits");
 const auto central_offset=static_cast<std::uint32_t>(out.size());
 for(const auto& entry:entries){zip_u32(out,0x02014b50);zip_u16(out,20);zip_u16(out,20);zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u32(out,entry.crc);zip_u32(out,static_cast<std::uint32_t>(entry.data.size()));zip_u32(out,static_cast<std::uint32_t>(entry.data.size()));zip_u16(out,static_cast<std::uint16_t>(entry.name.size()));zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u16(out,0);zip_u32(out,0);zip_u32(out,entry.offset);out+=entry.name;}
 const auto central_size=static_cast<std::uint32_t>(out.size()-central_offset);zip_u32(out,0x06054b50);zip_u16(out,0);zip_u16(out,0);zip_u16(out,static_cast<std::uint16_t>(entries.size()));zip_u16(out,static_cast<std::uint16_t>(entries.size()));zip_u32(out,central_size);zip_u32(out,central_offset);zip_u16(out,0);return out;
}
static std::string rand_id(){unsigned char b[12];if(RAND_bytes(b,sizeof(b))!=1)throw Failure("random generator unavailable");return hex(b,sizeof(b));}
static std::string normalize_email(std::string s){while(!s.empty()&&std::isspace(static_cast<unsigned char>(s.front())))s.erase(s.begin());while(!s.empty()&&std::isspace(static_cast<unsigned char>(s.back())))s.pop_back();for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
static std::string name_from_email(const std::string& email){
 auto at=email.find('@');std::string local=at==std::string::npos?email:email.substr(0,at);
 std::string name;bool cap=true;
 for(char c:local){if(c=='.'||c=='_'||c=='-'){name+=' ';cap=true;}else{name+=cap?static_cast<char>(std::toupper(static_cast<unsigned char>(c))):c;cap=false;}}
 return name.empty()?email:name;
}
static std::string json_extract_string(const std::string& body,const std::string& key){
 std::string pat="\""+key+"\"";auto pos=body.find(pat);if(pos==std::string::npos)return "";
 auto colon=body.find(':',pos+pat.size());if(colon==std::string::npos)return "";
 auto q1=body.find('"',colon+1);if(q1==std::string::npos)return "";
 std::string value;
 for(std::size_t i=q1+1;i<body.size();++i){
  char c=body[i];if(c=='"')return value;
  if(c!='\\'){value+=c;continue;}
  if(++i>=body.size())return "";
  char escaped=body[i];
  if(escaped=='"'||escaped=='\\'||escaped=='/')value+=escaped;
  else if(escaped=='b')value+='\b';else if(escaped=='f')value+='\f';else if(escaped=='n')value+='\n';else if(escaped=='r')value+='\r';else if(escaped=='t')value+='\t';
  else if(escaped=='u'){
   auto hex_value=[](char h)->int{if(h>='0'&&h<='9')return h-'0';if(h>='a'&&h<='f')return h-'a'+10;if(h>='A'&&h<='F')return h-'A'+10;return -1;};
   if(i+4>=body.size())return "";
   std::uint32_t codepoint=0;
   for(int n=0;n<4;++n){int digit=hex_value(body[++i]);if(digit<0)return "";codepoint=(codepoint<<4)|static_cast<std::uint32_t>(digit);}
   if(codepoint>=0xD800&&codepoint<=0xDBFF&&i+6<body.size()&&body[i+1]=='\\'&&body[i+2]=='u'){
    std::uint32_t low=0;bool valid=true;for(int n=0;n<4;++n){int digit=hex_value(body[i+3+n]);if(digit<0){valid=false;break;}low=(low<<4)|static_cast<std::uint32_t>(digit);}
    if(valid&&low>=0xDC00&&low<=0xDFFF){codepoint=0x10000+((codepoint-0xD800)<<10)+(low-0xDC00);i+=6;}
   }
   if(codepoint<=0x7F)value+=static_cast<char>(codepoint);
   else if(codepoint<=0x7FF){value+=static_cast<char>(0xC0|(codepoint>>6));value+=static_cast<char>(0x80|(codepoint&0x3F));}
   else if(codepoint<=0xFFFF){value+=static_cast<char>(0xE0|(codepoint>>12));value+=static_cast<char>(0x80|((codepoint>>6)&0x3F));value+=static_cast<char>(0x80|(codepoint&0x3F));}
   else{value+=static_cast<char>(0xF0|(codepoint>>18));value+=static_cast<char>(0x80|((codepoint>>12)&0x3F));value+=static_cast<char>(0x80|((codepoint>>6)&0x3F));value+=static_cast<char>(0x80|(codepoint&0x3F));}
  }else return "";
 }
 return "";
}
 static int json_extract_int(const std::string& body,const std::string& key,int fallback=-1){
  std::string pat="\""+key+"\"";auto pos=body.find(pat);if(pos==std::string::npos)return fallback;
  auto colon=body.find(':',pos+pat.size());if(colon==std::string::npos)return fallback;
  auto start=body.find_first_of("-0123456789",colon+1);if(start==std::string::npos)return fallback;
  try{return std::stoi(body.substr(start));}catch(...){return fallback;}
 }
static std::optional<double> json_extract_optional_double(const std::string& body,const std::string& key){
 const std::string needle="\""+key+"\"";auto p=body.find(needle);if(p==std::string::npos)return std::nullopt;
 p=body.find(':',p+needle.size());if(p==std::string::npos)throw Failure("Invalid JSON number");++p;
 while(p<body.size()&&std::isspace(static_cast<unsigned char>(body[p])))++p;
 if(body.compare(p,4,"null")==0)return std::nullopt;
 std::size_t used=0;double value=0;try{value=std::stod(body.substr(p),&used);}catch(...){throw Failure("Invalid JSON number: "+key);}
 if(used==0||!std::isfinite(value))throw Failure("Invalid JSON number: "+key);
 return value;
}
static std::vector<double> json_extract_number_array(const std::string& body,const std::string& key){
 const std::string needle="\""+key+"\"";auto p=body.find(needle);if(p==std::string::npos)return {};p=body.find('[',p+needle.size());if(p==std::string::npos)return {};const auto end=body.find(']',p+1);if(end==std::string::npos)throw Failure("Invalid JSON number array: "+key);std::vector<double> values;while(++p<end){while(p<end&&(std::isspace(static_cast<unsigned char>(body[p]))||body[p]==','))++p;if(p>=end)break;std::size_t used=0;double value=0;try{value=std::stod(body.substr(p,end-p),&used);}catch(...){throw Failure("Invalid JSON number array: "+key);}if(!used||!std::isfinite(value))throw Failure("Invalid JSON number array: "+key);values.push_back(value);p+=used-1;}return values;
}
static void put(const fs::path& p,const std::string& data){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary|std::ios::trunc);if(!f)throw Failure("could not create file");f.write(data.data(),std::streamsize(data.size()));f.flush();if(!f)throw Failure("could not write file");::chmod(p.c_str(),0600);}
static std::string slurp(const fs::path& p){std::ifstream f(p,std::ios::binary);if(!f)throw Failure("could not read file");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
using Db=rizoma::database::Connection;
using Stmt=rizoma::database::Statement;
static void sql(sqlite3* db,const char* statement){rizoma::database::execute(db,statement);}
static void initialize(const App& app){
 fs::create_directories(app.root/"blobs");fs::create_directories(app.root/"derived");fs::create_directories(app.root/"staging");::chmod(app.root.c_str(),0700);
 rizoma::database::migrate(app.root/"morfo.sqlite3");
 Db db(app.root/"morfo.sqlite3");
 morfo::profiles::create_schema(db.p);
}
static std::string detect_format(const std::string& bytes){
 if(bytes.size()>=8&&png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()),0,8)==0)return "png";
 if(bytes.size()>=3&& (unsigned char)bytes[0]==0xff&&(unsigned char)bytes[1]==0xd8&&(unsigned char)bytes[2]==0xff)return "jpeg";
 if(bytes.size()>=4&&
    (((unsigned char)bytes[0]==0x49&&(unsigned char)bytes[1]==0x49&&(unsigned char)bytes[2]==0x2a&&(unsigned char)bytes[3]==0x00)||
     ((unsigned char)bytes[0]==0x4d&&(unsigned char)bytes[1]==0x4d&&(unsigned char)bytes[2]==0x00&&(unsigned char)bytes[3]==0x2a)))return "tiff";
 throw Failure("Only PNG, JPEG, and TIFF images are accepted");
}
static Raster png_decode(const std::string& s){png_image p{};p.version=PNG_IMAGE_VERSION;if(!png_image_begin_read_from_memory(&p,s.data(),s.size()))throw Failure("Invalid PNG image");auto cleanup=[&]{png_image_free(&p);};if(p.width==0||p.height==0||std::uint64_t(p.width)*p.height>MAX_PIXELS){cleanup();throw Failure("Image exceeds 12 megapixels");}Raster r;r.w=int(p.width);r.h=int(p.height);p.format=PNG_FORMAT_RGBA;r.rgba.resize(PNG_IMAGE_SIZE(p));if(!png_image_finish_read(&p,nullptr,r.rgba.data(),0,nullptr)){cleanup();throw Failure("PNG could not be decoded");}cleanup();return r;}
struct JpegError{jpeg_error_mgr pub;jmp_buf jump;char message[JMSG_LENGTH_MAX]{};};
extern "C" void on_jpeg_error(j_common_ptr cinfo){auto* err=reinterpret_cast<JpegError*>(cinfo->err);(*cinfo->err->format_message)(cinfo,err->message);longjmp(err->jump,1);}
static Raster jpeg_decode(const std::string& s){jpeg_decompress_struct ci{};JpegError er{};ci.err=jpeg_std_error(&er.pub);er.pub.error_exit=on_jpeg_error;Raster r;std::vector<unsigned char>row;if(setjmp(er.jump)){jpeg_destroy_decompress(&ci);throw Failure(std::string("Invalid JPEG: ")+er.message);}jpeg_create_decompress(&ci);jpeg_mem_src(&ci,reinterpret_cast<const unsigned char*>(s.data()),s.size());jpeg_read_header(&ci,TRUE);if(ci.image_width==0||ci.image_height==0||std::uint64_t(ci.image_width)*ci.image_height>MAX_PIXELS){jpeg_destroy_decompress(&ci);throw Failure("Image exceeds 12 megapixels");}ci.out_color_space=JCS_RGB;jpeg_start_decompress(&ci);r.w=int(ci.output_width);r.h=int(ci.output_height);r.rgba.resize(std::size_t(r.w)*r.h*4);row.resize(std::size_t(r.w)*3);while(ci.output_scanline<ci.output_height){JSAMPROW rp=row.data();jpeg_read_scanlines(&ci,&rp,1);auto off=std::size_t(ci.output_scanline-1)*r.w*4;for(int x=0;x<r.w;x++){r.rgba[off+4*x]=row[3*x];r.rgba[off+4*x+1]=row[3*x+1];r.rgba[off+4*x+2]=row[3*x+2];r.rgba[off+4*x+3]=255;}}jpeg_finish_decompress(&ci);jpeg_destroy_decompress(&ci);return r;}
struct MemTiff{const unsigned char* data;tmsize_t size;tmsize_t pos;};
static tmsize_t tiff_read(thandle_t h,void* buf,tmsize_t s){auto* m=reinterpret_cast<MemTiff*>(h);if(m->pos>=m->size)return 0;tmsize_t avail=std::min(s,m->size-m->pos);std::memcpy(buf,m->data+m->pos,avail);m->pos+=avail;return avail;}
static tmsize_t tiff_write(thandle_t,void*,tmsize_t){return 0;}
static toff_t tiff_seek(thandle_t h,toff_t off,int whence){
 auto* m=reinterpret_cast<MemTiff*>(h);
 int64_t cur=static_cast<int64_t>(m->pos), len=static_cast<int64_t>(m->size), delta=static_cast<int64_t>(off);
 int64_t next=0;
 switch(whence){case SEEK_SET:next=delta;break;case SEEK_CUR:next=cur+delta;break;case SEEK_END:next=len+delta;break;default:return static_cast<toff_t>(-1);}
 if(next<0||next>len)return static_cast<toff_t>(-1);
 m->pos=static_cast<tmsize_t>(next);
 return static_cast<toff_t>(next);
}
static int tiff_close(thandle_t){return 0;}
static toff_t tiff_size(thandle_t h){auto* m=reinterpret_cast<MemTiff*>(h);return m->size;}
static Raster tiff_decode(const std::string& s){
 MemTiff mem{reinterpret_cast<const unsigned char*>(s.data()),static_cast<tmsize_t>(s.size()),0};
 TIFF* tif=TIFFClientOpen("mem","r",&mem,tiff_read,tiff_write,tiff_seek,tiff_close,tiff_size,nullptr,nullptr);
 if(!tif)throw Failure("Invalid TIFF image");
 uint32_t w=0,h=0;TIFFGetField(tif,TIFFTAG_IMAGEWIDTH,&w);TIFFGetField(tif,TIFFTAG_IMAGELENGTH,&h);
 if(w==0||h==0||std::uint64_t(w)*h>MAX_PIXELS){TIFFClose(tif);throw Failure("Image exceeds 12 megapixels");}
 std::vector<uint32_t> buf(std::size_t(w)*h);
 if(!TIFFReadRGBAImageOriented(tif,w,h,buf.data(),ORIENTATION_TOPLEFT,0)){TIFFClose(tif);throw Failure("TIFF could not be decoded");}
 TIFFClose(tif);Raster r;r.w=int(w);r.h=int(h);r.rgba.resize(std::size_t(w)*h*4);
 for(std::size_t i=0;i<buf.size();++i){uint32_t p=buf[i];r.rgba[i*4+0]=static_cast<unsigned char>(TIFFGetR(p));r.rgba[i*4+1]=static_cast<unsigned char>(TIFFGetG(p));r.rgba[i*4+2]=static_cast<unsigned char>(TIFFGetB(p));r.rgba[i*4+3]=static_cast<unsigned char>(TIFFGetA(p));}
 return r;
}
static Raster decode(const std::string& s,const std::string& fmt){
 if(fmt=="png")return png_decode(s);
 if(fmt=="jpeg")return jpeg_decode(s);
 return tiff_decode(s);
}
static std::string png_encode(const Raster& r){png_image im{};im.version=PNG_IMAGE_VERSION;im.width=r.w;im.height=r.h;im.format=PNG_FORMAT_RGBA;png_alloc_size_t n=0;if(!png_image_write_to_memory(&im,nullptr,&n,0,r.rgba.data(),0,nullptr))throw Failure("Preview encoding failed");std::string data(std::size_t(n),'\0');if(!png_image_write_to_memory(&im,data.data(),&n,0,r.rgba.data(),0,nullptr))throw Failure("Preview encoding failed");data.resize(std::size_t(n));return data;}
static Raster preview(const Raster& orig,bool gray,double& mean){double ratio=std::min(1.,double(THUMB_MAX)/std::max(orig.w,orig.h));Raster out;out.w=std::max(1,int(std::lround(orig.w*ratio)));out.h=std::max(1,int(std::lround(orig.h*ratio)));out.rgba.resize(std::size_t(out.w)*out.h*4);double total=0;for(int y=0;y<out.h;y++){int sy=std::min(orig.h-1,int(double(y)*orig.h/out.h));for(int x=0;x<out.w;x++){int sx=std::min(orig.w-1,int(double(x)*orig.w/out.w));auto i=(std::size_t(sy)*orig.w+sx)*4,j=(std::size_t(y)*out.w+x)*4;const unsigned char* c=&orig.rgba[i];unsigned char lum=static_cast<unsigned char>(std::clamp(std::lround(.2126*c[0]+.7152*c[1]+.0722*c[2]),0L,255L));total+=lum;if(gray){out.rgba[j]=out.rgba[j+1]=out.rgba[j+2]=lum;}else{out.rgba[j]=c[0];out.rgba[j+1]=c[1];out.rgba[j+2]=c[2];}out.rgba[j+3]=255;}}mean=total/(out.w*out.h);return out;}
struct PyramidLevel{std::uint64_t width=0,height=0;};
static std::vector<PyramidLevel> pyramid_levels(std::uint64_t width,std::uint64_t height){
 std::vector<PyramidLevel> result{{width,height}};
 while(result.back().width>TILE_SIZE||result.back().height>TILE_SIZE)
  result.push_back({std::max<std::uint64_t>(1,(result.back().width+1)/2),std::max<std::uint64_t>(1,(result.back().height+1)/2)});
 return result;
}
static std::string pyramid_manifest_json(const std::string& id,std::uint64_t width,std::uint64_t height){
 const auto levels=pyramid_levels(width,height);std::ostringstream out;
 out<<"{\"schema\":\"rizoma.image-pyramid.v1\",\"image_id\":"<<q(id)<<",\"tile_size\":"<<TILE_SIZE<<",\"width\":"<<width<<",\"height\":"<<height<<",\"levels\":[";
 bool first=true;for(std::size_t index=levels.size();index-->0;){if(!first)out<<',';first=false;const auto& level=levels[index];out<<"{\"level\":"<<index<<",\"width\":"<<level.width<<",\"height\":"<<level.height<<",\"columns\":"<<(level.width+TILE_SIZE-1)/TILE_SIZE<<",\"rows\":"<<(level.height+TILE_SIZE-1)/TILE_SIZE<<",\"url_template\":"<<q("/api/v1/images/"+id+"/tiles/"+std::to_string(index)+"/{x}/{y}.png")<<'}';}
 return out<<"]}",out.str();
}
static std::optional<rizoma::tiles::Pyramid> build_pyramid(const App& app,const std::string& id,const fs::path& source){
 const fs::path parent=app.root/"derived"/id,target=parent/"pyramid",complete=target/".complete";
 if(fs::exists(complete))return std::nullopt;
 static std::mutex pyramid_mutex;std::lock_guard lock(pyramid_mutex);
 if(fs::exists(complete))return std::nullopt;
 const fs::path temporary=parent/("pyramid-"+rand_id()+".tmp");
 try{
  const auto pyramid=rizoma::tiles::create(source,temporary,TILE_SIZE);
  put(temporary/"manifest.json",pyramid_manifest_json(id,pyramid.width,pyramid.height));put(temporary/".complete",utc_offset(0));
  if(fs::exists(target))fs::remove_all(target);
  fs::rename(temporary,target);
  return pyramid;
 }catch(...){std::error_code ignored;fs::remove_all(temporary,ignored);throw;}
}
static std::string safe_filename(std::string s){auto pos=s.find_last_of("/\\");if(pos!=std::string::npos)s=s.substr(pos+1);std::string n;for(unsigned char c:s){if(c>=32&&c!=127&&c!='"'&&c!='<'&&c!='>'&&c!='\\')n+=char(c);}if(n.empty())n="imagem-sem-nome";if(n.size()>100)n.resize(100);return n;}
static bool sample_ok(const std::string& s){return s.size()<=64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-'||c=='.';});}
static bool date_ok(const std::string& s){if(s.empty())return true;if(s.size()!=10)return false;for(int i=0;i<10;++i)if((i==4||i==7)?s[i]!='-':!std::isdigit(static_cast<unsigned char>(s[i])))return false;return s.substr(5,2)>="01"&&s.substr(5,2)<="12"&&s.substr(8,2)>="01"&&s.substr(8,2)<="31";}
static int nib(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static std::string decode_uri(const std::string& s){std::string v;for(std::size_t i=0;i<s.size();++i){if(s[i]=='%'&&i+2<s.size()&&nib(s[i+1])>=0&&nib(s[i+2])>=0){v+=char((nib(s[i+1])<<4)|nib(s[i+2]));i+=2;}else if(s[i]=='+')v+=' ';else v+=s[i];}return v;}
static std::map<std::string,std::string> params(const std::string& path){std::map<std::string,std::string> p;auto pos=path.find('?');if(pos==std::string::npos)return p;std::string tail=path.substr(pos+1);std::istringstream in(tail);std::string part;while(std::getline(in,part,'&')){auto eq=part.find('=');if(eq!=std::string::npos)p[decode_uri(part.substr(0,eq))]=decode_uri(part.substr(eq+1));}return p;}
static std::string col(sqlite3_stmt* st,int i){auto p=reinterpret_cast<const char*>(sqlite3_column_text(st,i));return p?p:"";}
static std::string row_json(sqlite3_stmt* st){std::ostringstream o;o<<"{\"id\":"<<q(col(st,0))<<",\"filename\":"<<q(col(st,1))<<",\"sample_id\":"<<q(col(st,2))<<",\"capture_date\":"<<q(col(st,3))<<",\"created_at\":"<<q(col(st,4))<<",\"sha256\":"<<q(col(st,5))<<",\"bytes\":"<<sqlite3_column_int64(st,6)<<",\"width\":"<<sqlite3_column_int64(st,7)<<",\"height\":"<<sqlite3_column_int64(st,8)<<",\"format\":"<<q(col(st,9))<<",\"brightness\":"<<std::fixed<<std::setprecision(1)<<sqlite3_column_double(st,10)<<",\"qc_state\":"<<q(col(st,11))<<",\"pipeline\":"<<q(col(st,12))<<",\"user_email\":"<<q(col(st,13))<<",\"user_id\":"<<q(col(st,14))<<",\"scientific_status\":\"NOT_ANALYZED\"}";return o.str();}
constexpr const char* fields="id,filename,sample_id,capture_date,created_at,sha256,bytes,width,height,format,brightness,qc_state,pipeline,user_email,user_id";
static std::string all_images(const App& app,const std::string& user_id){
 Db d(app.root/"morfo.sqlite3");
 auto stm=std::string("SELECT ")+fields+" FROM images WHERE user_id=? AND deleted_at IS NULL ORDER BY created_at DESC, id DESC LIMIT 500";
 Stmt st(d.p,stm.c_str());
 st.bind(1,user_id);
 std::string out="{\"schema\":\"rizoma.images.v0.2.0\",\"data\":[";
 bool first=true;
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)out+=",";first=false;out+=row_json(st.p);}
 out+="]}";
 return out;
}
static std::string one_image(const App& app,const std::string& id,const std::string& user_id){
 Db d(app.root/"morfo.sqlite3");
 auto stm=std::string("SELECT ")+fields+" FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL";
 Stmt st(d.p,stm.c_str());
 st.bind(1,id);
 st.bind(2,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Image not found");
 return "{\"data\":"+row_json(st.p)+"}";
}
static std::string update_image_metadata(const App& app,const std::string& id,const std::string& user_id,const std::string& body){
 const std::string filename=json_extract_string(body,"filename");
 const std::string sample_id=json_extract_string(body,"sample_id");
 const std::string capture_date=json_extract_string(body,"capture_date");
 if(!sample_id.empty()&&!sample_ok(sample_id))throw Failure("Invalid sample identifier");
 if(!capture_date.empty()&&!date_ok(capture_date))throw Failure("Invalid capture date");

 Db db(app.root/"morfo.sqlite3");
 Stmt check(db.p,"SELECT filename,sample_id,capture_date FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 check.bind(1,id);check.bind(2,user_id);
 if(sqlite3_step(check.p)!=SQLITE_ROW)throw Failure("Image not found");

 const std::string new_fn=filename.empty()?col(check.p,0):safe_filename(filename);
 const std::string new_sample=sample_id.empty()?col(check.p,1):sample_id;
 const std::string new_date=capture_date.empty()?col(check.p,2):capture_date;

 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt update(db.p,"UPDATE images SET filename=?, sample_id=?, capture_date=? WHERE id=? AND user_id=? AND deleted_at IS NULL");
  update.bind(1,new_fn);update.bind(2,new_sample);update.bind(3,new_date);update.bind(4,id);update.bind(5,user_id);
  if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

  Stmt ev(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'IMAGE_METADATA_UPDATED',?,?)");
  ev.bind(1,id);ev.bind(2,utc_offset(0));ev.bind(3,"filename="+new_fn+" | sample_id="+new_sample+" | capture_date="+new_date);
  sqlite3_step(ev.p);
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}

 return one_image(app,id,user_id);
}
static std::string delete_image(const App& app,const std::string& id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt check(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 check.bind(1,id);check.bind(2,user_id);
 if(sqlite3_step(check.p)!=SQLITE_ROW)throw Failure("Image not found");

 const std::string now=utc_offset(0);
 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt update(db.p,"UPDATE images SET deleted_at=? WHERE id=? AND user_id=? AND deleted_at IS NULL");
  update.bind(1,now);update.bind(2,id);update.bind(3,user_id);
  if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

  Stmt upd_runs(db.p,"UPDATE analysis_runs SET deleted_at=? WHERE image_id=? AND deleted_at IS NULL");
  upd_runs.bind(1,now);upd_runs.bind(2,id);
  sqlite3_step(upd_runs.p);

  Stmt ev(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'IMAGE_SOFT_DELETED',?,?)");
  ev.bind(1,id);ev.bind(2,now);ev.bind(3,"deleted_by_user="+user_id);
  sqlite3_step(ev.p);
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}

 return "{\"data\":{\"id\":"+q(id)+",\"deleted\":true,\"deleted_at\":"+q(now)+"}}";
}
static std::string csv_cell(const std::string& s){std::string x=s;if(!x.empty()&&(x[0]=='='||x[0]=='+'||x[0]=='-'||x[0]=='@'))x="'"+x;std::string out="\"";for(char c:x){if(c=='"')out+="\"\"";else if(c!='\r'&&c!='\n')out+=c;}return out+"\"";}
static std::string manifest(const App& app,const std::string& user_id){
 Db d(app.root/"morfo.sqlite3");
 Stmt st(d.p,"SELECT id,filename,sample_id,capture_date,created_at,sha256,bytes,width,height,format,qc_state,pipeline,user_email FROM images WHERE user_id=? AND deleted_at IS NULL ORDER BY created_at DESC");
 st.bind(1,user_id);
 std::string r="id,filename,sample_id,capture_date,created_at,sha256,bytes,width,height,format,qc_state,pipeline,user_email,scientific_status\r\n";
 while(sqlite3_step(st.p)==SQLITE_ROW){
  for(int i=0;i<13;++i){
   if(i)r+=",";
   r+=csv_cell(col(st.p,i));
  }
  r+=",\"NOT_ANALYZED\"\r\n";
 }
 return r;
}
static std::string media(const App& app,const std::string& id,const std::string& kind,std::string& mime,const std::string& user_id){
 Db d(app.root/"morfo.sqlite3");Stmt st(d.p,"SELECT original_path,preview_path,grayscale_path,format,width,height FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 st.bind(1,id);
 st.bind(2,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Image not found");
 std::string fmt=col(st.p,3);
 if(kind=="original-view"){
  const auto width=sqlite3_column_int64(st.p,4),height=sqlite3_column_int64(st.p,5);
  if(width<=0||height<=0||static_cast<std::uint64_t>(width)>MAX_PIXELS/static_cast<std::uint64_t>(height)){
   mime="image/png";return slurp(app.root/col(st.p,1));
  }
  if(fmt=="tiff"){
   fs::path view_path = app.root/("derived/"+id+"/original_view.png");
   if(!fs::exists(view_path)){
    std::string raw = slurp(app.root/col(st.p,0));
    Raster r = tiff_decode(raw);
    put(view_path, png_encode(r));
   }
   mime="image/png";
   return slurp(view_path);
  }
  fs::path orig_path = app.root/col(st.p,0);
  mime = fmt=="jpeg" ? "image/jpeg" : "image/png";
  return slurp(orig_path);
 }
 int i=kind=="original"?0:kind=="preview"?1:2;
 fs::path stored=col(st.p,i); // paths are generated by this service, never from clients
 if(kind=="original"){
  if(fmt=="png")mime="image/png";
  else if(fmt=="jpeg")mime="image/jpeg";
  else if(fmt=="tiff")mime="image/tiff";
  else mime="application/octet-stream";
 }else mime="image/png";
 return slurp(app.root/stored);
}
struct AuthorizedImage{std::string original_path,format;std::uint64_t width=0,height=0;};
static AuthorizedImage authorized_image(const App& app,const std::string& id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt image(db.p,"SELECT original_path,format,width,height FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");image.bind(1,id);image.bind(2,user_id);
 if(sqlite3_step(image.p)!=SQLITE_ROW)throw Failure("Image not found");
 return {col(image.p,0),col(image.p,1),static_cast<std::uint64_t>(sqlite3_column_int64(image.p,2)),static_cast<std::uint64_t>(sqlite3_column_int64(image.p,3))};
}
static AuthorizedImage ensure_pyramid(const App& app,const std::string& id,const std::string& user_id){
 auto image=authorized_image(app,id,user_id);
 if(!fs::exists(app.root/"derived"/id/"pyramid"/".complete"))build_pyramid(app,id,app.root/image.original_path);
 return image;
}
static std::string pyramid_manifest(const App& app,const std::string& id,const std::string& user_id){
 const auto image=ensure_pyramid(app,id,user_id);return pyramid_manifest_json(id,image.width,image.height);
}
static bool nonnegative_integer(std::string_view text,std::uint64_t& value){
 if(text.empty())return false;
 value=0;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);return error==std::errc{}&&end==text.data()+text.size();
}
static std::string pyramid_tile(const App& app,const std::string& id,const std::string& resource,std::string& mime,const std::string& user_id){
 const auto image=ensure_pyramid(app,id,user_id);const auto levels=pyramid_levels(image.width,image.height);
 // resource is tiles/{level}/{x}/{y}.png; every component is parsed before a path is assembled.
 std::vector<std::string> parts;std::istringstream stream(resource);std::string part;while(std::getline(stream,part,'/'))parts.push_back(part);
 if(parts.size()!=4||parts[0]!="tiles"||!parts[3].ends_with(".png"))throw Failure("Tile not found");
 parts[3].resize(parts[3].size()-4);std::uint64_t level=0,x=0,y=0;
 if(!nonnegative_integer(parts[1],level)||!nonnegative_integer(parts[2],x)||!nonnegative_integer(parts[3],y)||level>=levels.size())throw Failure("Tile not found");
 const auto& selected=levels[std::size_t(level)];
 if(x>=(selected.width+TILE_SIZE-1)/TILE_SIZE||y>=(selected.height+TILE_SIZE-1)/TILE_SIZE)throw Failure("Tile not found");
 mime="image/png";return slurp(app.root/"derived"/id/"pyramid"/("L"+std::to_string(level))/(std::to_string(x)+"_"+std::to_string(y)+".png"));
}
struct Ingested{std::string id;std::string json;};
static Ingested ingest_file(const App& app,const fs::path& staged,const std::string& filename,const std::string& sample,const std::string& captured,const std::string& user_id,const std::string& user_email){
 if(!sample_ok(sample)||!date_ok(captured))throw Failure("Invalid sample identifier or capture date");
 std::error_code size_error;const auto bytes=fs::file_size(staged,size_error);if(size_error||bytes==0)throw Failure("Image must contain data");
 std::ifstream header(staged,std::ios::binary);std::string signature(8,'\0');header.read(signature.data(),signature.size());signature.resize(static_cast<std::size_t>(header.gcount()));
 const auto fmt=detect_format(signature),hash=hash256_file(staged);
 Db db(app.root/"morfo.sqlite3");{
  Stmt lookup(db.p,"SELECT id FROM images WHERE sha256=? AND user_id=? AND deleted_at IS NULL");
  lookup.bind(1,hash);
  lookup.bind(2,user_id);
  if(sqlite3_step(lookup.p)==SQLITE_ROW)throw Failure("An identical original is already registered in your collection (SHA-256 duplicate)");
 }
 const std::string id=rand_id();const std::string ext=fmt=="png"?"png":fmt=="jpeg"?"jpg":"tif";
 struct DerivedGuard {fs::path path;bool keep=false;~DerivedGuard(){if(!keep){std::error_code ignored;fs::remove_all(path,ignored);}}} derived_guard{app.root/"derived"/id};
 const std::string orig=std::string("blobs/")+hash.substr(0,2)+"/"+hash+"."+ext;
 const std::string a="derived/"+id+"/preview.png",b="derived/"+id+"/grayscale.png";
 const auto created=build_pyramid(app,id,staged);if(!created)throw Failure("Pyramid publication conflict");const auto& pyramid=*created;
 if(pyramid.source_sha256!=hash)throw Failure("Source changed while preparing pyramid");
 const auto top_level=pyramid.levels.size()-1;const auto thumbbytes=slurp(app.root/"derived"/id/"pyramid"/("L"+std::to_string(top_level))/"0_0.png");
 auto thumbnail=png_decode(thumbbytes);double brightness=0,unused=0;auto color=preview(thumbnail,false,brightness);auto gray=preview(thumbnail,true,unused);
 put(app.root/a,png_encode(color));put(app.root/b,png_encode(gray));
 fs::create_directories((app.root/orig).parent_path());if(!fs::exists(app.root/orig))fs::rename(staged,app.root/orig);else fs::remove(staged);
 const std::string state=brightness<35?"LOW_LIGHT":brightness>220?"HIGH_LIGHT":"REVIEW";
 sql(db.p,"BEGIN IMMEDIATE");try{
 Stmt st(db.p,"INSERT INTO images(id,filename,sample_id,capture_date,created_at,sha256,bytes,width,height,format,brightness,qc_state,pipeline,original_path,preview_path,grayscale_path,user_email,user_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
 st.bind(1,id);st.bind(2,safe_filename(filename));st.bind(3,sample);st.bind(4,captured);st.bind(5,utc_offset(0));st.bind(6,hash);sqlite3_bind_int64(st.p,7,static_cast<sqlite3_int64>(bytes));sqlite3_bind_int64(st.p,8,static_cast<sqlite3_int64>(pyramid.width));sqlite3_bind_int64(st.p,9,static_cast<sqlite3_int64>(pyramid.height));st.bind(10,fmt);sqlite3_bind_double(st.p,11,brightness);st.bind(12,state);st.bind(13,PIPELINE);st.bind(14,orig);st.bind(15,a);st.bind(16,b);st.bind(17,user_email);st.bind(18,user_id);
 if(sqlite3_step(st.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
 Stmt ev(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'IMAGE_RECEIVED_PROGRESSIVE',?,?)");ev.bind(1,id);ev.bind(2,utc_offset(0));ev.bind(3,hash+" | "+user_id+" | "+user_email);if(sqlite3_step(ev.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));sql(db.p,"COMMIT");}catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
 derived_guard.keep=true;return {id,one_image(app,id,user_id)};
}
constexpr const char* ANALYSIS_ALGORITHM="rizoma-exp-002-003-integrated-baseline";
constexpr const char* ANALYSIS_VERSION="0.1.0-experimental";
constexpr const char* ANALYSIS_PARAMS="{\"global_sigma\":1.2,\"local_sigma\":1.45,\"cell\":32,\"min_area\":4,\"min_aspect\":1.7,\"execution\":\"in_memory_current_ingest_domain\"}";

static std::string analysis_json(const App& app,const std::string& image_id,const std::string& user_id,const std::string& requested_run=""){
 Db db(app.root/"morfo.sqlite3");
 Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");owned.bind(1,image_id);owned.bind(2,user_id);
 if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");
 const std::string query=requested_run.empty()
  ?"SELECT id,algorithm,algorithm_version,parameters_json,source_sha256,status,created_at,completed_at,COALESCE(notes,'') FROM analysis_runs WHERE image_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC LIMIT 1"
  :"SELECT id,algorithm,algorithm_version,parameters_json,source_sha256,status,created_at,completed_at,COALESCE(notes,'') FROM analysis_runs WHERE image_id=? AND id=? AND deleted_at IS NULL";
 Stmt run(db.p,query.c_str());run.bind(1,image_id);if(!requested_run.empty())run.bind(2,requested_run);
 if(sqlite3_step(run.p)!=SQLITE_ROW)return "{\"data\":null}";
 const std::string run_id=col(run.p,0);std::ostringstream out;
 out<<"{\"data\":{\"id\":"<<q(run_id)<<",\"algorithm\":"<<q(col(run.p,1))<<",\"algorithm_version\":"<<q(col(run.p,2))
    <<",\"parameters\":"<<col(run.p,3)<<",\"source_sha256\":"<<q(col(run.p,4))<<",\"status\":"<<q(col(run.p,5))
    <<",\"created_at\":"<<q(col(run.p,6))<<",\"completed_at\":"<<q(col(run.p,7))<<",\"notes\":"<<q(col(run.p,8))<<",\"regions\":[";
 Stmt regions(db.p,R"SQL(SELECT r.id,r.ordinal,r.x,r.y,r.width,r.height,r.area,r.evidence_json,r.current_version,
  j.label,COALESCE(e.action,''),COALESCE(e.label,''),COALESCE(e.justification,''),COALESCE(e.created_at,''),j.evidence_json
 FROM analysis_regions r
 JOIN analysis_judgments j ON j.id = (
   SELECT j2.id FROM analysis_judgments j2
   WHERE j2.region_id=r.id AND j2.actor_type='ALGORITHM'
   ORDER BY j2.created_at DESC, j2.rowid DESC LIMIT 1
 )
 LEFT JOIN review_events e ON e.region_id=r.id AND e.version=r.current_version
 WHERE r.run_id=? ORDER BY r.ordinal)SQL");regions.bind(1,run_id);bool first=true;
 int total_count=0,root_count=0,matrix_count=0,artifact_count=0,unknown_count=0,reviewed_count=0;
 std::int64_t total_root_area=0,total_region_area=0;
 while(sqlite3_step(regions.p)==SQLITE_ROW){
  if(!first)out<<',';
  first=false;
  total_count++;
  const auto area=sqlite3_column_int64(regions.p,6);
  total_region_area+=area;
  const std::string rev_act=col(regions.p,10),rev_lbl=col(regions.p,11),sugg_lbl=col(regions.p,9);
  const std::string final_lbl=rev_lbl.empty()?sugg_lbl:rev_lbl;
  if(!rev_act.empty())reviewed_count++;
  if(final_lbl=="ROOT"){root_count++;total_root_area+=area;}
  else if(final_lbl=="MATRIX")matrix_count++;
  else if(final_lbl=="ARTIFACT")artifact_count++;
  else unknown_count++;

  const std::string jev=col(regions.p,14);
  out<<"{\"id\":"<<q(col(regions.p,0))<<",\"ordinal\":"<<sqlite3_column_int(regions.p,1)
     <<",\"x\":"<<sqlite3_column_int64(regions.p,2)<<",\"y\":"<<sqlite3_column_int64(regions.p,3)
     <<",\"width\":"<<sqlite3_column_int64(regions.p,4)<<",\"height\":"<<sqlite3_column_int64(regions.p,5)
     <<",\"area\":"<<area<<",\"evidence\":"<<col(regions.p,7)
     <<",\"version\":"<<sqlite3_column_int(regions.p,8)<<",\"suggestion\":"<<q(sugg_lbl)
     <<",\"judgment_evidence\":"<<(jev.empty()?"{}":jev)<<",\"review\":";
  if(rev_act.empty())out<<"null";else out<<"{\"action\":"<<q(rev_act)<<",\"label\":"<<q(rev_lbl)<<",\"justification\":"<<q(col(regions.p,12))<<",\"created_at\":"<<q(col(regions.p,13))<<"}";
  out<<'}';
 }
 out<<"],\"statistics\":{"
    <<"\"total_regions\":"<<total_count
    <<",\"root_count\":"<<root_count
    <<",\"matrix_count\":"<<matrix_count
    <<",\"artifact_count\":"<<artifact_count
    <<",\"unknown_count\":"<<unknown_count
    <<",\"reviewed_count\":"<<reviewed_count
    <<",\"pending_count\":"<<(total_count-reviewed_count)
    <<",\"total_root_area\":"<<total_root_area
    <<",\"total_region_area\":"<<total_region_area
    <<"}}}";
 return out.str();
}

static std::string analysis_export_csv(const App& app,const std::string& image_id,const std::string& user_id,const std::string& requested_run=""){
 Db db(app.root/"morfo.sqlite3");
 Stmt img(db.p,"SELECT filename,sample_id,capture_date,width,height,sha256,format FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 img.bind(1,image_id);img.bind(2,user_id);
 if(sqlite3_step(img.p)!=SQLITE_ROW)throw Failure("Image not found");

 const std::string filename=col(img.p,0),sample_id=col(img.p,1),capture_date=col(img.p,2);
 const auto width=sqlite3_column_int64(img.p,3),height=sqlite3_column_int64(img.p,4);
 const std::string sha256=col(img.p,5),format=col(img.p,6);
 const auto total_img_area=width*height;

 const std::string query=requested_run.empty()
  ?"SELECT id,algorithm,algorithm_version,status,created_at,completed_at,COALESCE(notes,'') FROM analysis_runs WHERE image_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC LIMIT 1"
  :"SELECT id,algorithm,algorithm_version,status,created_at,completed_at,COALESCE(notes,'') FROM analysis_runs WHERE image_id=? AND id=? AND deleted_at IS NULL";
 Stmt run(db.p,query.c_str());run.bind(1,image_id);if(!requested_run.empty())run.bind(2,requested_run);
 if(sqlite3_step(run.p)!=SQLITE_ROW)throw Failure("Analysis not found");

 const std::string run_id=col(run.p,0),algo=col(run.p,1),algo_ver=col(run.p,2);
 const std::string status=col(run.p,3),created_at=col(run.p,4),completed_at=col(run.p,5),notes=col(run.p,6);

 Stmt regions(db.p,R"SQL(SELECT r.ordinal,r.x,r.y,r.width,r.height,r.area,
  j.label,COALESCE(e.label,''),COALESCE(e.action,''),COALESCE(e.justification,''),
  COALESCE(e.created_at,''),COALESCE(u.email,''),j.evidence_json
 FROM analysis_regions r
 JOIN analysis_judgments j ON j.id = (
   SELECT j2.id FROM analysis_judgments j2
   WHERE j2.region_id=r.id AND j2.actor_type='ALGORITHM'
   ORDER BY j2.created_at DESC, j2.rowid DESC LIMIT 1
 )
 LEFT JOIN review_events e ON e.region_id=r.id AND e.version=r.current_version
 LEFT JOIN users u ON u.id=e.reviewer_user_id
 WHERE r.run_id=? ORDER BY r.ordinal)SQL");
 regions.bind(1,run_id);

 struct CsvRow {
  int ordinal;
  std::int64_t x,y,w,h,area;
  std::string algo_label,final_label,action,justification,review_at,reviewer;
  double confidence;
 };
 std::vector<CsvRow> rows;
 int root_count=0,matrix_count=0,artifact_count=0,unknown_count=0,reviewed_count=0;
 std::int64_t total_root_area=0,total_region_area=0;

 while(sqlite3_step(regions.p)==SQLITE_ROW){
  CsvRow row;
  row.ordinal=sqlite3_column_int(regions.p,0);
  row.x=sqlite3_column_int64(regions.p,1);
  row.y=sqlite3_column_int64(regions.p,2);
  row.w=sqlite3_column_int64(regions.p,3);
  row.h=sqlite3_column_int64(regions.p,4);
  row.area=sqlite3_column_int64(regions.p,5);
  row.algo_label=col(regions.p,6);
  std::string rev_label=col(regions.p,7);
  row.action=col(regions.p,8);
  row.justification=col(regions.p,9);
  row.review_at=col(regions.p,10);
  row.reviewer=col(regions.p,11);
  row.final_label=rev_label.empty()?row.algo_label:rev_label;

  std::string jev=col(regions.p,12);
  row.confidence=0.0;
  auto pos=jev.find("\"confidence\":");
  if(pos!=std::string::npos){
   try{row.confidence=std::stod(jev.substr(pos+13));}catch(...){}
  }

  if(!row.action.empty())reviewed_count++;
  if(row.final_label=="ROOT"){root_count++;total_root_area+=row.area;}
  else if(row.final_label=="MATRIX")matrix_count++;
  else if(row.final_label=="ARTIFACT")artifact_count++;
  else unknown_count++;
  total_region_area+=row.area;

  rows.push_back(std::move(row));
 }

 const double density_pct = total_img_area > 0 ? (double(total_root_area) / double(total_img_area)) * 100.0 : 0.0;
 const double root_coverage_of_regions = total_region_area > 0 ? (double(total_root_area) / double(total_region_area)) * 100.0 : 0.0;

 std::ostringstream out;
 out<<"# MORFO - Relatorio Cientifico e Estatisticas Radiculares\r\n"
    <<"# Amostra,"<<csv_cell(sample_id)<<"\r\n"
    <<"# Arquivo,"<<csv_cell(filename)<<"\r\n"
    <<"# Formato,"<<format<<",Largura,"<<width<<",Altura,"<<height<<",Area Total Imagem (px),"<<total_img_area<<"\r\n"
    <<"# SHA-256,"<<sha256<<"\r\n"
    <<"# Algoritmo,"<<csv_cell(algo)<<",Versao,"<<csv_cell(algo_ver)<<"\r\n"
    <<"# Status Analise,"<<status<<",Data Execucao,"<<created_at<<",Data Conclusao,"<<completed_at<<"\r\n"
    <<"# Notas,"<<csv_cell(notes)<<"\r\n"
    <<"# \r\n"
    <<"# SUMARIO ESTATISTICO RADICULAR:\r\n"
    <<"# Total Regioes Analisadas,"<<rows.size()<<"\r\n"
    <<"# Regioes ROOT (Raizes),"<<root_count<<"\r\n"
    <<"# Regioes MATRIX (Solo/Matriz),"<<matrix_count<<"\r\n"
    <<"# Regioes ARTIFACT (Pedras/Artefatos),"<<artifact_count<<"\r\n"
    <<"# Regioes UNKNOWN (Desconhecidas),"<<unknown_count<<"\r\n"
    <<"# Regioes com Revisao Humana,"<<reviewed_count<<" ("<<(rows.empty()?0:(reviewed_count*100/rows.size()))<<"%)\r\n"
    <<"# Area Total de Raizes (px),"<<total_root_area<<"\r\n"
    <<"# Densidade Radicular (% area da imagem),"<<std::fixed<<std::setprecision(3)<<density_pct<<"%\r\n"
    <<"# Proporcao de Raizes sobre Regioes Detectadas,"<<std::fixed<<std::setprecision(2)<<root_coverage_of_regions<<"%\r\n"
    <<"# \r\n"
    <<"ordinal,x,y,width,height,area_pixels,label,confidence,status_revisao,revisor,justificativa,data_revisao\r\n";

 for(const auto& r : rows){
  out<<r.ordinal<<","<<r.x<<","<<r.y<<","<<r.w<<","<<r.h<<","<<r.area<<","
     <<csv_cell(r.final_label)<<","<<std::fixed<<std::setprecision(2)<<r.confidence<<","
     <<(r.action.empty()?"AUTOMATICO":r.action)<<","
     <<csv_cell(r.reviewer)<<","<<csv_cell(r.justification)<<","<<r.review_at<<"\r\n";
 }
 return out.str();
}

static std::string analysis_runs_json(const App& app,const std::string& image_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");owned.bind(1,image_id);owned.bind(2,user_id);
 if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");
 Stmt runs(db.p,R"SQL(SELECT a.id,a.algorithm_version,a.status,a.created_at,a.completed_at,COUNT(r.id),COALESCE(a.notes,'')
 FROM analysis_runs a LEFT JOIN analysis_regions r ON r.run_id=a.id WHERE a.image_id=? AND a.deleted_at IS NULL
 GROUP BY a.id ORDER BY a.created_at DESC,a.rowid DESC)SQL");runs.bind(1,image_id);
 std::string out="{\"data\":[";bool first=true;while(sqlite3_step(runs.p)==SQLITE_ROW){if(!first)out+=',';first=false;out+="{\"id\":"+q(col(runs.p,0))+",\"algorithm_version\":"+q(col(runs.p,1))+",\"status\":"+q(col(runs.p,2))+",\"created_at\":"+q(col(runs.p,3))+",\"completed_at\":"+q(col(runs.p,4))+",\"region_count\":"+std::to_string(sqlite3_column_int64(runs.p,5))+",\"notes\":"+q(col(runs.p,6))+"}";}return out+"]}";
}

static std::string evaluation_overview_json(const App& app,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 auto count=[&](const char* sql){Stmt st(db.p,sql);st.bind(1,user_id);if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Cannot read evaluation overview");return sqlite3_column_int64(st.p,0);};
 const auto protocols=count("SELECT COUNT(*) FROM evaluation_protocols WHERE owner_user_id=?");
 const auto datasets=count("SELECT COUNT(*) FROM evaluation_datasets d JOIN evaluation_protocols p ON p.id=d.protocol_id WHERE p.owner_user_id=?");
 const auto runs=count("SELECT COUNT(*) FROM evaluation_runs r JOIN evaluation_protocols p ON p.id=r.protocol_id WHERE p.owner_user_id=?");
 const auto completed=count("SELECT COUNT(*) FROM evaluation_runs r JOIN evaluation_protocols p ON p.id=r.protocol_id WHERE p.owner_user_id=? AND r.status='COMPLETED'");
 Stmt summaries(db.p,R"SQL(
SELECT s.id,s.run_id,s.metric,s.estimate,s.ci_lower,s.ci_upper,s.sample_count,s.method,s.created_at,
       r.algorithm,r.algorithm_version,p.title
 FROM evaluation_summaries s JOIN evaluation_runs r ON r.id=s.run_id
 JOIN evaluation_protocols p ON p.id=r.protocol_id
 WHERE p.owner_user_id=? ORDER BY s.created_at DESC,s.rowid DESC LIMIT 50
)SQL");summaries.bind(1,user_id);
 std::ostringstream out;out<<"{\"schema\":\"morfo.evaluation-overview.v1\",\"data\":{\"counts\":{\"protocols\":"<<protocols<<",\"datasets\":"<<datasets<<",\"runs\":"<<runs<<",\"completed\":"<<completed<<"},\"summaries\":[";
 bool first=true;while(sqlite3_step(summaries.p)==SQLITE_ROW){if(!first)out<<',';first=false;out<<"{\"id\":"<<q(col(summaries.p,0))<<",\"run_id\":"<<q(col(summaries.p,1))<<",\"metric\":"<<q(col(summaries.p,2))<<",\"estimate\":"<<sqlite3_column_double(summaries.p,3)<<",\"ci_lower\":"<<(sqlite3_column_type(summaries.p,4)==SQLITE_NULL?"null":std::to_string(sqlite3_column_double(summaries.p,4)))<<",\"ci_upper\":"<<(sqlite3_column_type(summaries.p,5)==SQLITE_NULL?"null":std::to_string(sqlite3_column_double(summaries.p,5)))<<",\"sample_count\":"<<sqlite3_column_int64(summaries.p,6)<<",\"method\":"<<q(col(summaries.p,7))<<",\"created_at\":"<<q(col(summaries.p,8))<<",\"algorithm\":"<<q(col(summaries.p,9))<<",\"algorithm_version\":"<<q(col(summaries.p,10))<<",\"protocol\":"<<q(col(summaries.p,11))<<'}';}
 out<<"]}}";return out.str();
}

static std::string evaluation_protocols_json(const App& app,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt rows(db.p,R"SQL(
SELECT id,title,hypothesis,task,experimental_unit,split_strategy,acceptance_criteria_json,
       limitations,state,protocol_version,created_at
 FROM evaluation_protocols WHERE owner_user_id=? ORDER BY created_at DESC,rowid DESC
)SQL");rows.bind(1,user_id);std::ostringstream out;out<<"{\"schema\":\"morfo.evaluation-protocols.v1\",\"data\":[";bool first=true;
 while(sqlite3_step(rows.p)==SQLITE_ROW){if(!first)out<<',';first=false;out<<"{\"id\":"<<q(col(rows.p,0))<<",\"title\":"<<q(col(rows.p,1))<<",\"hypothesis\":"<<q(col(rows.p,2))<<",\"task\":"<<q(col(rows.p,3))<<",\"experimental_unit\":"<<q(col(rows.p,4))<<",\"split_strategy\":"<<q(col(rows.p,5))<<",\"acceptance_criteria\":"<<col(rows.p,6)<<",\"limitations\":"<<q(col(rows.p,7))<<",\"state\":"<<q(col(rows.p,8))<<",\"version\":"<<sqlite3_column_int(rows.p,9)<<",\"created_at\":"<<q(col(rows.p,10))<<'}';}
 out<<"]}";return out.str();
}

static std::string evaluation_datasets_json(const App& app,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt datasets(db.p,R"SQL(
SELECT d.id,d.protocol_id,d.name,d.content_sha256,d.reference_kind,d.frozen_at,d.created_at,d.state,p.title
 FROM evaluation_datasets d JOIN evaluation_protocols p ON p.id=d.protocol_id
 WHERE p.owner_user_id=? ORDER BY d.created_at DESC,d.rowid DESC
)SQL");datasets.bind(1,user_id);std::ostringstream out;out<<"{\"schema\":\"morfo.evaluation-datasets.v1\",\"data\":[";bool first_dataset=true;
 while(sqlite3_step(datasets.p)==SQLITE_ROW){if(!first_dataset)out<<',';first_dataset=false;const std::string dataset_id=col(datasets.p,0);out<<"{\"id\":"<<q(dataset_id)<<",\"protocol_id\":"<<q(col(datasets.p,1))<<",\"name\":"<<q(col(datasets.p,2))<<",\"content_sha256\":"<<q(col(datasets.p,3))<<",\"reference_kind\":"<<q(col(datasets.p,4))<<",\"frozen_at\":"<<(col(datasets.p,5).empty()?"null":q(col(datasets.p,5)))<<",\"created_at\":"<<q(col(datasets.p,6))<<",\"state\":"<<q(col(datasets.p,7))<<",\"protocol\":"<<q(col(datasets.p,8))<<",\"units\":[";
  Stmt units(db.p,R"SQL(SELECT u.id,u.external_key,u.split,u.group_key,u.input_sha256,u.metadata_json,u.image_id,u.profile_run_id,i.filename,i.sample_id,u.reference_sha256,u.reference_filename,u.reference_created_at FROM evaluation_units u LEFT JOIN images i ON i.id=u.image_id WHERE u.dataset_id=? ORDER BY u.rowid)SQL");units.bind(1,dataset_id);bool first_unit=true;while(sqlite3_step(units.p)==SQLITE_ROW){if(!first_unit)out<<',';first_unit=false;out<<"{\"id\":"<<q(col(units.p,0))<<",\"external_key\":"<<q(col(units.p,1))<<",\"split\":"<<q(col(units.p,2))<<",\"group_key\":"<<q(col(units.p,3))<<",\"input_sha256\":"<<q(col(units.p,4))<<",\"metadata\":"<<col(units.p,5)<<",\"image_id\":"<<q(col(units.p,6))<<",\"profile_run_id\":"<<q(col(units.p,7))<<",\"filename\":"<<q(col(units.p,8))<<",\"sample_id\":"<<q(col(units.p,9))<<",\"reference_sha256\":"<<(col(units.p,10).empty()?"null":q(col(units.p,10)))<<",\"reference_filename\":"<<(col(units.p,11).empty()?"null":q(col(units.p,11)))<<",\"reference_created_at\":"<<(col(units.p,12).empty()?"null":q(col(units.p,12)))<<'}';}out<<"]}";
 }
 out<<"]}";return out.str();
}

static void refresh_evaluation_manifest(sqlite3* db,const std::string& dataset_id){
 Stmt manifest(db,"SELECT input_sha256,split,group_key,profile_run_id,COALESCE(reference_sha256,'') FROM evaluation_units WHERE dataset_id=? ORDER BY external_key");manifest.bind(1,dataset_id);std::string material;while(sqlite3_step(manifest.p)==SQLITE_ROW)material+=col(manifest.p,0)+"|"+col(manifest.p,1)+"|"+col(manifest.p,2)+"|"+col(manifest.p,3)+"|"+col(manifest.p,4)+"\n";Stmt update(db,"UPDATE evaluation_datasets SET content_sha256=? WHERE id=? AND state='DRAFT'");update.bind(1,hash256_text(material));update.bind(2,dataset_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db)!=1)throw Failure("Draft evaluation dataset not found");
}

static std::string update_evaluation_unit(const App& app,const std::string& user_id,const std::string& dataset_id,const std::string& unit_id,const std::string& body){
 const auto split=json_extract_string(body,"split"),group=json_extract_string(body,"group_key");if(split!="DEVELOPMENT"&&split!="TEST")throw Failure("Split must be DEVELOPMENT or TEST");if(group.empty()||group.size()>160)throw Failure("Experimental group is required");Db db(app.root/"morfo.sqlite3");Stmt update(db.p,"UPDATE evaluation_units SET split=?,group_key=? WHERE id=? AND dataset_id=? AND EXISTS(SELECT 1 FROM evaluation_datasets d JOIN evaluation_protocols p ON p.id=d.protocol_id WHERE d.id=? AND d.state='DRAFT' AND p.owner_user_id=?)");update.bind(1,split);update.bind(2,group);update.bind(3,unit_id);update.bind(4,dataset_id);update.bind(5,dataset_id);update.bind(6,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Draft evaluation unit not found");refresh_evaluation_manifest(db.p,dataset_id);return evaluation_datasets_json(app,user_id);
}

static std::string delete_evaluation_unit(const App& app,const std::string& user_id,const std::string& dataset_id,const std::string& unit_id){
 Db db(app.root/"morfo.sqlite3");Stmt path(db.p,"SELECT reference_relative_path FROM evaluation_units u JOIN evaluation_datasets d ON d.id=u.dataset_id JOIN evaluation_protocols p ON p.id=d.protocol_id WHERE u.id=? AND d.id=? AND d.state='DRAFT' AND p.owner_user_id=?");path.bind(1,unit_id);path.bind(2,dataset_id);path.bind(3,user_id);if(sqlite3_step(path.p)!=SQLITE_ROW)throw Failure("Draft evaluation unit not found");const auto relative=col(path.p,0);Stmt remove(db.p,"DELETE FROM evaluation_units WHERE id=? AND dataset_id=?");remove.bind(1,unit_id);remove.bind(2,dataset_id);if(sqlite3_step(remove.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Draft evaluation unit not found");refresh_evaluation_manifest(db.p,dataset_id);if(!relative.empty()){std::error_code ec;fs::remove(app.root/relative,ec);}return evaluation_datasets_json(app,user_id);
}

static std::string add_evaluation_reference(const App& app,const std::string& user_id,const std::string& dataset_id,const std::string& unit_id,const std::string& filename,const std::string& bytes){
 if(bytes.empty())throw Failure("Reference mask is empty");
 if(bytes.size()>MAX_CONTROL_BODY)throw Failure("Reference mask exceeds 1 MiB");
 const auto reference=png_decode(bytes);Db db(app.root/"morfo.sqlite3");
 Stmt inspect(db.p,"SELECT i.width,i.height FROM evaluation_units u JOIN evaluation_datasets d ON d.id=u.dataset_id JOIN evaluation_protocols p ON p.id=d.protocol_id JOIN images i ON i.id=u.image_id WHERE u.id=? AND d.id=? AND d.state='DRAFT' AND p.owner_user_id=?");inspect.bind(1,unit_id);inspect.bind(2,dataset_id);inspect.bind(3,user_id);if(sqlite3_step(inspect.p)!=SQLITE_ROW)throw Failure("Draft evaluation unit not found");
 if(reference.w!=sqlite3_column_int(inspect.p,0)||reference.h!=sqlite3_column_int(inspect.p,1))throw Failure("Reference mask dimensions must match original image");
 const auto sha=hash256_text(bytes),relative=(fs::path("references")/dataset_id/(unit_id+".png")).generic_string();put(app.root/relative,bytes);
 try{Stmt update(db.p,"UPDATE evaluation_units SET reference_sha256=?,reference_relative_path=?,reference_filename=?,reference_created_by_user_id=?,reference_created_at=? WHERE id=? AND dataset_id=?");update.bind(1,sha);update.bind(2,relative);update.bind(3,filename.empty()?"reference.png":filename);update.bind(4,user_id);update.bind(5,utc_offset(0));update.bind(6,unit_id);update.bind(7,dataset_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Cannot register reference mask");Stmt kind(db.p,"UPDATE evaluation_datasets SET reference_kind='SPECIALIST' WHERE id=? AND state='DRAFT'");kind.bind(1,dataset_id);if(sqlite3_step(kind.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));refresh_evaluation_manifest(db.p,dataset_id);return evaluation_datasets_json(app,user_id);}catch(...){std::error_code ec;fs::remove(app.root/relative,ec);throw;}
}

static std::string create_evaluation_protocol(const App& app,const std::string& user_id,const std::string& body){
 const auto title=json_extract_string(body,"title"),hypothesis=json_extract_string(body,"hypothesis"),task=json_extract_string(body,"task"),unit=json_extract_string(body,"experimental_unit"),split=json_extract_string(body,"split_strategy"),criteria=json_extract_string(body,"acceptance_criteria"),limitations=json_extract_string(body,"limitations"),state=json_extract_string(body,"state");
 const std::set<std::string> tasks={"CLASSIFICATION","SEGMENTATION","CLUSTERING","MEASUREMENT","ROBUSTNESS","REPRODUCIBILITY"};
 if(title.empty()||title.size()>160)throw Failure("Protocol title must contain 1..160 characters");
 if(hypothesis.empty()||hypothesis.size()>2000)throw Failure("Protocol hypothesis must contain 1..2000 characters");
 if(!tasks.contains(task))throw Failure("Invalid evaluation task");
 if(unit.empty()||unit.size()>160)throw Failure("Experimental unit is required");
 if(split.empty()||split.size()>500)throw Failure("Split strategy is required");
 if(criteria.empty()||criteria.size()>2000)throw Failure("Acceptance criteria are required");
 if(limitations.size()>2000)throw Failure("Limitations exceed 2000 characters");
 if(state!="DRAFT"&&state!="FROZEN")throw Failure("Protocol state must be DRAFT or FROZEN");
 Db db(app.root/"morfo.sqlite3");Stmt version(db.p,"SELECT COALESCE(MAX(protocol_version),0)+1 FROM evaluation_protocols WHERE owner_user_id=? AND title=?");version.bind(1,user_id);version.bind(2,title);if(sqlite3_step(version.p)!=SQLITE_ROW)throw Failure("Cannot version protocol");const int next=sqlite3_column_int(version.p,0);
 const std::string id=rand_id(),now=utc_offset(0),criteria_json="{\"statement\":"+q(criteria)+"}";Stmt insert(db.p,"INSERT INTO evaluation_protocols(id,owner_user_id,title,hypothesis,task,experimental_unit,split_strategy,acceptance_criteria_json,limitations,state,protocol_version,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");insert.bind(1,id);insert.bind(2,user_id);insert.bind(3,title);insert.bind(4,hypothesis);insert.bind(5,task);insert.bind(6,unit);insert.bind(7,split);insert.bind(8,criteria_json);insert.bind(9,limitations);insert.bind(10,state);sqlite3_bind_int(insert.p,11,next);insert.bind(12,now);if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
 return evaluation_protocols_json(app,user_id);
}

static std::string freeze_evaluation_protocol(const App& app,const std::string& user_id,const std::string& protocol_id){
 Db db(app.root/"morfo.sqlite3");Stmt drafts(db.p,"SELECT COUNT(*) FROM evaluation_datasets d JOIN evaluation_protocols p ON p.id=d.protocol_id WHERE d.protocol_id=? AND p.owner_user_id=? AND d.state='DRAFT'");drafts.bind(1,protocol_id);drafts.bind(2,user_id);if(sqlite3_step(drafts.p)!=SQLITE_ROW)throw Failure("Cannot inspect evaluation datasets");if(sqlite3_column_int(drafts.p,0)>0)throw Failure("Freeze evaluation datasets before freezing protocol");Stmt update(db.p,"UPDATE evaluation_protocols SET state='FROZEN' WHERE id=? AND owner_user_id=? AND state='DRAFT'");update.bind(1,protocol_id);update.bind(2,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));if(sqlite3_changes(db.p)!=1){Stmt exists(db.p,"SELECT state FROM evaluation_protocols WHERE id=? AND owner_user_id=?");exists.bind(1,protocol_id);exists.bind(2,user_id);if(sqlite3_step(exists.p)!=SQLITE_ROW)throw Failure("Evaluation protocol not found");throw Failure("Protocol is already frozen");}return evaluation_protocols_json(app,user_id);
}

static std::string freeze_evaluation_dataset(const App& app,const std::string& user_id,const std::string& dataset_id){
 Db db(app.root/"morfo.sqlite3");Stmt inspect(db.p,"SELECT d.state,(SELECT COUNT(*) FROM evaluation_units u WHERE u.dataset_id=d.id),(SELECT COUNT(*) FROM evaluation_units u WHERE u.dataset_id=d.id AND (u.reference_sha256 IS NULL OR u.reference_sha256='')),p.task FROM evaluation_datasets d JOIN evaluation_protocols p ON p.id=d.protocol_id WHERE d.id=? AND p.owner_user_id=?");inspect.bind(1,dataset_id);inspect.bind(2,user_id);if(sqlite3_step(inspect.p)!=SQLITE_ROW)throw Failure("Evaluation dataset not found");if(col(inspect.p,0)!="DRAFT")throw Failure("Dataset is already frozen");if(sqlite3_column_int(inspect.p,1)<1)throw Failure("Empty dataset cannot be frozen");if(col(inspect.p,3)=="SEGMENTATION"&&sqlite3_column_int(inspect.p,2)>0)throw Failure("Every unit requires an independent reference mask before freezing");Stmt update(db.p,"UPDATE evaluation_datasets SET state='FROZEN',frozen_at=? WHERE id=? AND state='DRAFT'");update.bind(1,utc_offset(0));update.bind(2,dataset_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Cannot freeze evaluation dataset");return evaluation_datasets_json(app,user_id);
}

static std::string attach_profile_to_evaluation(const App& app,const std::string& user_id,const std::string& protocol_id,const std::string& body){
 const auto profile_run_id=json_extract_string(body,"profile_run_id"),split=json_extract_string(body,"split"),group_key=json_extract_string(body,"group_key");if(profile_run_id.size()!=24||!std::all_of(profile_run_id.begin(),profile_run_id.end(),[](char c){return(c>='a'&&c<='f')||(c>='0'&&c<='9');}))throw Failure("Invalid profile run identifier");if(split!="DEVELOPMENT"&&split!="TEST")throw Failure("Split must be DEVELOPMENT or TEST");if(group_key.empty()||group_key.size()>160)throw Failure("Experimental group is required");
 Db db(app.root/"morfo.sqlite3");Stmt protocol(db.p,"SELECT title,state FROM evaluation_protocols WHERE id=? AND owner_user_id=?");protocol.bind(1,protocol_id);protocol.bind(2,user_id);if(sqlite3_step(protocol.p)!=SQLITE_ROW)throw Failure("Evaluation protocol not found");if(col(protocol.p,1)!="DRAFT")throw Failure("Frozen protocol cannot receive new units");
 Stmt profile(db.p,R"SQL(SELECT r.image_id,r.original_sha256,r.mask_sha256,r.version,i.filename,i.sample_id FROM morfo_profile_runs r JOIN images i ON i.id=r.image_id WHERE r.id=? AND r.user_id=? AND r.deleted_at IS NULL AND i.user_id=? AND i.deleted_at IS NULL)SQL");profile.bind(1,profile_run_id);profile.bind(2,user_id);profile.bind(3,user_id);if(sqlite3_step(profile.p)!=SQLITE_ROW)throw Failure("Profile run not found");const std::string image_id=col(profile.p,0),input_sha=col(profile.p,1),mask_sha=col(profile.p,2),version=col(profile.p,3),filename=col(profile.p,4),sample=col(profile.p,5);
 sql(db.p,"BEGIN IMMEDIATE");try{std::string dataset_id;Stmt dataset(db.p,"SELECT id FROM evaluation_datasets WHERE protocol_id=? AND state='DRAFT' ORDER BY created_at DESC LIMIT 1");dataset.bind(1,protocol_id);if(sqlite3_step(dataset.p)==SQLITE_ROW)dataset_id=col(dataset.p,0);else{dataset_id=rand_id();const auto now=utc_offset(0);Stmt create(db.p,"INSERT INTO evaluation_datasets(id,protocol_id,name,content_sha256,reference_kind,frozen_at,created_at,state) VALUES(?,?,?,'PENDING','NONE','',?,'DRAFT')");create.bind(1,dataset_id);create.bind(2,protocol_id);create.bind(3,"Conjunto operacional — "+col(protocol.p,0));create.bind(4,now);if(sqlite3_step(create.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));}
  const std::string unit_id=rand_id(),metadata="{\"filename\":"+q(filename)+",\"sample_id\":"+q(sample)+",\"profile_version\":"+q(version)+",\"mask_sha256\":"+q(mask_sha)+"}";Stmt unit(db.p,"INSERT INTO evaluation_units(id,dataset_id,external_key,split,group_key,input_sha256,metadata_json,image_id,profile_run_id) VALUES(?,?,?,?,?,?,?,?,?)");unit.bind(1,unit_id);unit.bind(2,dataset_id);unit.bind(3,image_id);unit.bind(4,split);unit.bind(5,group_key);unit.bind(6,input_sha);unit.bind(7,metadata);unit.bind(8,image_id);unit.bind(9,profile_run_id);if(sqlite3_step(unit.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
  Stmt manifest(db.p,"SELECT input_sha256,split,group_key,profile_run_id FROM evaluation_units WHERE dataset_id=? ORDER BY external_key");manifest.bind(1,dataset_id);std::string material;int count=0;while(sqlite3_step(manifest.p)==SQLITE_ROW){material+=col(manifest.p,0)+"|"+col(manifest.p,1)+"|"+col(manifest.p,2)+"|"+col(manifest.p,3)+"\n";++count;}Stmt update(db.p,"UPDATE evaluation_datasets SET content_sha256=? WHERE id=? AND state='DRAFT'");update.bind(1,hash256_text(material));update.bind(2,dataset_id);if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));sql(db.p,"COMMIT");return "{\"data\":{\"dataset_id\":"+q(dataset_id)+",\"unit_id\":"+q(unit_id)+",\"profile_run_id\":"+q(profile_run_id)+",\"split\":"+q(split)+",\"group_key\":"+q(group_key)+",\"unit_count\":"+std::to_string(count)+"}}";}catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

static std::string update_analysis_run(const App& app,const std::string& run_id,const std::string& user_id,const std::string& body){
 const std::string status=json_extract_string(body,"status");
 const std::string notes=json_extract_string(body,"notes");
 const std::set<std::string> valid_statuses={"REVIEW_REQUIRED","COMPLETED","ARCHIVED","RUNNING","FAILED"};
 if(!status.empty()&&!valid_statuses.contains(status))throw Failure("Invalid analysis status");
 if(notes.size()>2000)throw Failure("Notes exceed limit");

 Db db(app.root/"morfo.sqlite3");
 Stmt check(db.p,R"SQL(
  SELECT a.id,a.image_id,a.status,a.notes,a.completed_at
  FROM analysis_runs a JOIN images i ON i.id=a.image_id
  WHERE a.id=? AND i.user_id=? AND a.deleted_at IS NULL
 )SQL");
 check.bind(1,run_id);check.bind(2,user_id);
 if(sqlite3_step(check.p)!=SQLITE_ROW)throw Failure("Analysis run not found");

 const std::string image_id=col(check.p,1);
 const std::string cur_status=col(check.p,2);
 const std::string cur_notes=col(check.p,3);
 std::string completed_at=col(check.p,4);

 const std::string new_status=status.empty()?cur_status:status;
 const std::string new_notes=notes.empty()?cur_notes:notes;
 const std::string now=utc_offset(0);
 if(new_status=="COMPLETED"&&completed_at.empty())completed_at=now;

 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt update(db.p,"UPDATE analysis_runs SET status=?, notes=?, completed_at=? WHERE id=?");
  update.bind(1,new_status);update.bind(2,new_notes);update.bind(3,completed_at);update.bind(4,run_id);
  if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

  Stmt ev(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'ANALYSIS_UPDATED',?,?)");
  ev.bind(1,image_id);ev.bind(2,now);ev.bind(3,"run_id="+run_id+" | status="+new_status);
  sqlite3_step(ev.p);
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}

 return "{\"data\":{\"id\":"+q(run_id)+",\"image_id\":"+q(image_id)+",\"status\":"+q(new_status)+",\"notes\":"+q(new_notes)+",\"completed_at\":"+q(completed_at)+"}}";
}

static std::string delete_analysis_run(const App& app,const std::string& run_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt check(db.p,R"SQL(
  SELECT a.id,a.image_id
  FROM analysis_runs a JOIN images i ON i.id=a.image_id
  WHERE a.id=? AND i.user_id=? AND a.deleted_at IS NULL
 )SQL");
 check.bind(1,run_id);check.bind(2,user_id);
 if(sqlite3_step(check.p)!=SQLITE_ROW)throw Failure("Analysis run not found");
 const std::string image_id=col(check.p,1);

 const std::string now=utc_offset(0);
 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt update(db.p,"UPDATE analysis_runs SET deleted_at=? WHERE id=? AND deleted_at IS NULL");
  update.bind(1,now);update.bind(2,run_id);
  if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

  Stmt ev(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'ANALYSIS_SOFT_DELETED',?,?)");
  ev.bind(1,image_id);ev.bind(2,now);ev.bind(3,"run_id="+run_id);
  sqlite3_step(ev.p);
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}

 return "{\"data\":{\"id\":"+q(run_id)+",\"image_id\":"+q(image_id)+",\"deleted\":true,\"deleted_at\":"+q(now)+"}}";
}

static std::string confirm_all_reviews(const App& app,const std::string& run_id,const std::string& user_id,const std::string& body){
 Db db(app.root/"morfo.sqlite3");
 std::string justification=body.empty()?"":json_extract_string(body,"justification");
 if(justification.empty())justification="Aprovação em lote por inspeção visual global";
 if(justification.size()>1000)throw Failure("Justification too long");

 sql(db.p,"BEGIN IMMEDIATE");
 try{
  Stmt auth(db.p,R"SQL(
   SELECT a.id FROM analysis_runs a
   JOIN images i ON i.id=a.image_id
   WHERE a.id=? AND i.user_id=? AND a.deleted_at IS NULL AND i.deleted_at IS NULL
  )SQL");
  auth.bind(1,run_id);auth.bind(2,user_id);
  if(sqlite3_step(auth.p)!=SQLITE_ROW)throw Failure("Analysis run not found");

  Stmt pend(db.p,R"SQL(
   SELECT r.id, r.current_version, COALESCE(e.label, j.label)
   FROM analysis_regions r
   JOIN analysis_judgments j ON j.id = (
     SELECT j2.id FROM analysis_judgments j2
     WHERE j2.region_id=r.id AND j2.actor_type='ALGORITHM'
     ORDER BY j2.created_at DESC, j2.rowid DESC LIMIT 1
   )
   LEFT JOIN review_events e ON e.region_id=r.id AND e.version=r.current_version
   WHERE r.run_id=? AND e.id IS NULL
   ORDER BY r.ordinal
  )SQL");
  pend.bind(1,run_id);

  struct PendingReg { std::string id; int version; std::string label; };
  std::vector<PendingReg> to_confirm;
  while(sqlite3_step(pend.p)==SQLITE_ROW){
   to_confirm.push_back({col(pend.p,0),sqlite3_column_int(pend.p,1),col(pend.p,2)});
  }

  const std::string now=utc_offset(0);
  int confirmed_count=0;
  for(const auto& reg:to_confirm){
   const int next_ver=reg.version+1;
   const std::string ev_id=rand_id();
   Stmt ins_ev(db.p,"INSERT INTO review_events(id,region_id,reviewer_user_id,version,action,label,justification,created_at) VALUES(?,?,?,?,?,?,?,?)");
   ins_ev.bind(1,ev_id);ins_ev.bind(2,reg.id);ins_ev.bind(3,user_id);sqlite3_bind_int(ins_ev.p,4,next_ver);
   ins_ev.bind(5,"CONFIRM");ins_ev.bind(6,reg.label);ins_ev.bind(7,justification);ins_ev.bind(8,now);
   if(sqlite3_step(ins_ev.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

   Stmt upd_r(db.p,"UPDATE analysis_regions SET current_version=? WHERE id=? AND current_version=?");
   sqlite3_bind_int(upd_r.p,1,next_ver);upd_r.bind(2,reg.id);sqlite3_bind_int(upd_r.p,3,reg.version);
   if(sqlite3_step(upd_r.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Review conflict during batch confirmation");

   confirmed_count++;
  }

  Stmt log(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(NULL,'BATCH_CONFIRM',?,?)");
  log.bind(1,now);log.bind(2,"Confirmed "+std::to_string(confirmed_count)+" regions for run "+run_id);
  sqlite3_step(log.p);

  sql(db.p,"COMMIT");
  std::ostringstream out;
  out<<"{\"status\":\"ok\",\"confirmed_count\":"<<confirmed_count<<"}";
  return out.str();
 }catch(...){sql(db.p,"ROLLBACK");throw;}
}

static std::string start_analysis(const App& app,const std::string& image_id,const std::string& user_id,const std::string& algo_req=""){
 Db db(app.root/"morfo.sqlite3");
 Stmt image(db.p,"SELECT original_path,format,sha256,width,height FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");image.bind(1,image_id);image.bind(2,user_id);
 if(sqlite3_step(image.p)!=SQLITE_ROW)throw Failure("Image not found");
 const std::string path=col(image.p,0),format=col(image.p,1),source_sha=col(image.p,2);
 const auto width=sqlite3_column_int64(image.p,3),height=sqlite3_column_int64(image.p,4);
 if(width<=0||height<=0||static_cast<std::uint64_t>(width)>MAX_PIXELS/static_cast<std::uint64_t>(height))
  throw Failure("Analysis remains limited to 12 megapixels; EXP-004 large-image analysis is not implemented");
 auto raster=decode(slurp(app.root/path),format);
 auto result=rizoma::analysis::detect(static_cast<std::uint64_t>(raster.w),static_cast<std::uint64_t>(raster.h),raster.rgba);
 const std::string run_id=rand_id(),now=utc_offset(0);
 const auto strategy=rizoma::learning::parse_strategy(algo_req);
 const std::string algo_name=rizoma::learning::strategy_name(strategy);
 const std::string algo_version=strategy==rizoma::learning::Strategy::BASELINE_HEURISTIC?ANALYSIS_VERSION:"learning-v1.0";
 const std::string algo_params="{\"strategy\":"+q(algo_name)+",\"tile_size\":32}";
 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt insert_run(db.p,"INSERT INTO analysis_runs(id,image_id,requested_by_user_id,algorithm,algorithm_version,parameters_json,source_sha256,status,created_at,completed_at,error) VALUES(?,?,?,?,?,?,?,'REVIEW_REQUIRED',?,?,NULL)");
  insert_run.bind(1,run_id);insert_run.bind(2,image_id);insert_run.bind(3,user_id);insert_run.bind(4,algo_name);insert_run.bind(5,algo_version);insert_run.bind(6,algo_params);insert_run.bind(7,source_sha);insert_run.bind(8,now);insert_run.bind(9,now);
  if(sqlite3_step(insert_run.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
  int ordinal=0;for(const auto& region:result.regions){
   const std::string region_id=rand_id(),judgment_id=rand_id();++ordinal;
   std::ostringstream evidence;evidence<<"{\"global_pixels\":"<<region.global_pixels<<",\"local_pixels\":"<<region.local_pixels<<",\"shared_pixels\":"<<region.shared_pixels<<",\"touches_border\":"<<(region.touches_border?"true":"false")<<",\"global_mean\":"<<std::fixed<<std::setprecision(3)<<result.global_mean<<",\"global_stddev\":"<<result.global_stddev<<"}";
   Stmt insert_region(db.p,"INSERT INTO analysis_regions(id,run_id,ordinal,x,y,width,height,area,evidence_json,current_version,created_at) VALUES(?,?,?,?,?,?,?,?,?,0,?)");
   insert_region.bind(1,region_id);insert_region.bind(2,run_id);sqlite3_bind_int(insert_region.p,3,ordinal);sqlite3_bind_int64(insert_region.p,4,sqlite3_int64(region.x));sqlite3_bind_int64(insert_region.p,5,sqlite3_int64(region.y));sqlite3_bind_int64(insert_region.p,6,sqlite3_int64(region.width));sqlite3_bind_int64(insert_region.p,7,sqlite3_int64(region.height));sqlite3_bind_int64(insert_region.p,8,sqlite3_int64(region.area));insert_region.bind(9,evidence.str());insert_region.bind(10,now);
   if(sqlite3_step(insert_region.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));

   rizoma::learning::JudgmentSuggestion suggestion;
   if(strategy==rizoma::learning::Strategy::BAYESIAN_CALIBRATED){
    suggestion=rizoma::learning::infer_bayesian(db.p,user_id,region,result.global_mean,result.global_stddev);
   }else if(strategy==rizoma::learning::Strategy::TABULAR_INCREMENTAL){
    auto feat=rizoma::learning::extract_features(region,raster.w,raster.h);
    suggestion=rizoma::learning::infer_tabular(db.p,user_id,feat);
   }else if(strategy==rizoma::learning::Strategy::VISUAL_KNN){
    auto desc=rizoma::learning::extract_descriptor(raster.rgba,raster.w,raster.h,region);
    suggestion=rizoma::learning::infer_visual_knn(db.p,user_id,desc);
   }else{
    suggestion.label=region.shared_pixels>0?"ROOT":"UNKNOWN";
    suggestion.confidence=region.shared_pixels>0?0.75:0.60;
    suggestion.details_json="{\"strategy\":\"BASELINE_HEURISTIC\"}";
   }

   std::string combined_evidence="{\"evidence\":"+evidence.str()+",\"learning\":"+suggestion.details_json+",\"confidence\":"+std::to_string(suggestion.confidence)+"}";
   Stmt judgment(db.p,"INSERT INTO analysis_judgments(id,region_id,actor_type,label,evidence_json,created_at) VALUES(?,?,'ALGORITHM',?,?,?)");judgment.bind(1,judgment_id);judgment.bind(2,region_id);judgment.bind(3,suggestion.label);judgment.bind(4,combined_evidence);judgment.bind(5,now);
   if(sqlite3_step(judgment.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
  }
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
 return analysis_json(app,image_id,user_id);
}

static std::string review_history(const App& app,const std::string& region_id,const std::string& user_id,int recalibrated_count=0){
 Db db(app.root/"morfo.sqlite3");
 Stmt owned(db.p,R"SQL(SELECT 1 FROM analysis_regions r JOIN analysis_runs a ON a.id=r.run_id JOIN images i ON i.id=a.image_id WHERE r.id=? AND i.user_id=?)SQL");owned.bind(1,region_id);owned.bind(2,user_id);
 if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Region not found");
 Stmt history(db.p,R"SQL(SELECT e.id,e.version,e.action,e.label,e.justification,e.created_at,u.email
 FROM review_events e JOIN users u ON u.id=e.reviewer_user_id JOIN analysis_regions r ON r.id=e.region_id
 JOIN analysis_runs a ON a.id=r.run_id JOIN images i ON i.id=a.image_id
 WHERE e.region_id=? AND i.user_id=? ORDER BY e.version)SQL");history.bind(1,region_id);history.bind(2,user_id);
 std::string out="{\"data\":[";bool first=true;while(sqlite3_step(history.p)==SQLITE_ROW){if(!first)out+=',';first=false;out+="{\"id\":"+q(col(history.p,0))+",\"version\":"+std::to_string(sqlite3_column_int(history.p,1))+",\"action\":"+q(col(history.p,2))+",\"label\":"+q(col(history.p,3))+",\"justification\":"+q(col(history.p,4))+",\"created_at\":"+q(col(history.p,5))+",\"reviewer\":"+q(col(history.p,6))+"}";}
 out+="],\"recalibrated_count\":"+std::to_string(recalibrated_count)+"}";
 return out;
}

static std::string add_review(const App& app,const std::string& region_id,const std::string& user_id,const std::string& body){
 const std::string action=json_extract_string(body,"action"),label=json_extract_string(body,"label"),justification=json_extract_string(body,"justification");
 const int expected=json_extract_int(body,"expected_version");
 const std::set<std::string> actions={"CONFIRM","CORRECT","SUSPEND"},labels={"ROOT","MATRIX","ARTIFACT","UNKNOWN"};
 if(!actions.contains(action)||!labels.contains(label)||expected<0||justification.size()>1000)throw Failure("Invalid review");
 if(action=="SUSPEND"&&label!="UNKNOWN")throw Failure("Suspended reviews must use UNKNOWN");
 int recalibrated_count=0;
 Db db(app.root/"morfo.sqlite3");sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt region(db.p,R"SQL(SELECT r.current_version,a.image_id,COALESCE(e.label,j.label),r.x,r.y,r.width,r.height,r.area,r.evidence_json,
   i.original_path,i.format,i.width,i.height,r.run_id,
   CAST(COALESCE(json_extract(r.evidence_json,'$.global_pixels'),0) AS INT),
   CAST(COALESCE(json_extract(r.evidence_json,'$.local_pixels'),0) AS INT),
   CAST(COALESCE(json_extract(r.evidence_json,'$.shared_pixels'),0) AS INT),
   CAST(COALESCE(json_extract(r.evidence_json,'$.touches_border'),0) AS INT)
   FROM analysis_regions r JOIN analysis_runs a ON a.id=r.run_id JOIN images i ON i.id=a.image_id
   JOIN analysis_judgments j ON j.id = (
     SELECT j2.id FROM analysis_judgments j2
     WHERE j2.region_id=r.id AND j2.actor_type='ALGORITHM'
     ORDER BY j2.created_at DESC, j2.rowid DESC LIMIT 1
   )
   LEFT JOIN review_events e ON e.region_id=r.id AND e.version=r.current_version
   WHERE r.id=? AND i.user_id=?)SQL");region.bind(1,region_id);region.bind(2,user_id);
  if(sqlite3_step(region.p)!=SQLITE_ROW)throw Failure("Region not found");
  const int current_version=sqlite3_column_int(region.p,0);
  if(current_version!=expected)throw Failure("Review conflict");
  std::string final_action = action;
  if(action=="CONFIRM"&&label!=col(region.p,2)){
    final_action = "CORRECT";
  }

  const std::uint64_t reg_x=sqlite3_column_int64(region.p,3),reg_y=sqlite3_column_int64(region.p,4);
  const std::uint64_t reg_w=sqlite3_column_int64(region.p,5),reg_h=sqlite3_column_int64(region.p,6);
  const std::uint64_t reg_area=sqlite3_column_int64(region.p,7);
  const std::string orig_path=col(region.p,9),orig_fmt=col(region.p,10);
  const auto img_w=sqlite3_column_int64(region.p,11),img_h=sqlite3_column_int64(region.p,12);
  const std::string run_id=col(region.p,13);
  const auto glob_px=sqlite3_column_int64(region.p,14);
  const auto loc_px=sqlite3_column_int64(region.p,15);
  const auto shared_px=sqlite3_column_int64(region.p,16);
  const bool touches_b=sqlite3_column_int(region.p,17)!=0;

  const int next=current_version+1;const std::string event_id=rand_id(),now=utc_offset(0);
  Stmt event(db.p,"INSERT INTO review_events(id,region_id,reviewer_user_id,version,action,label,justification,created_at) VALUES(?,?,?,?,?,?,?,?)");event.bind(1,event_id);event.bind(2,region_id);event.bind(3,user_id);sqlite3_bind_int(event.p,4,next);event.bind(5,final_action);event.bind(6,label);event.bind(7,justification);event.bind(8,now);
  if(sqlite3_step(event.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));
  Stmt update(db.p,"UPDATE analysis_regions SET current_version=? WHERE id=? AND current_version=?");sqlite3_bind_int(update.p,1,next);update.bind(2,region_id);sqlite3_bind_int(update.p,3,current_version);
  if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Review conflict");

  if(final_action=="CONFIRM"||final_action=="CORRECT"){
   const bool is_root=(label=="ROOT");
   rizoma::analysis::Region r_obj{reg_x,reg_y,reg_w,reg_h,reg_area,
     static_cast<std::uint64_t>(glob_px),
     static_cast<std::uint64_t>(loc_px),
     static_cast<std::uint64_t>(shared_px),
     touches_b};
   auto feat=rizoma::learning::extract_features(r_obj,img_w,img_h);
   rizoma::learning::train_tabular_step(db.p,user_id,feat,is_root);
   std::optional<Raster> raster_cache;
   auto get_raster = [&]() -> const Raster& {
    if(!raster_cache) raster_cache = decode(slurp(app.root/orig_path),orig_fmt);
    return *raster_cache;
   };
   try{
    auto desc=rizoma::learning::extract_descriptor(get_raster().rgba,img_w,img_h,r_obj);
    rizoma::learning::record_visual_exemplar(db.p,user_id,desc,label);
   }catch(...){}

   const std::vector<unsigned char>* rgba_ptr = raster_cache ? &raster_cache->rgba : nullptr;
   auto rec_res = rizoma::learning::recalibrate_pending_regions(
     db.p, user_id, run_id, region_id, now,
     static_cast<std::uint64_t>(img_w), static_cast<std::uint64_t>(img_h),
     rgba_ptr
   );
   recalibrated_count = rec_res.recalibrated_count;
  }

  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
 return review_history(app,region_id,user_id,recalibrated_count);
}
using Response=http::response<http::string_body>;
static Response respond(http::status s,unsigned version,std::string body,std::string mime="application/json; charset=utf-8"){
 Response r{s,version};r.set(http::field::server,"MORFO/0.1.0");r.set(http::field::content_type,mime);r.set(http::field::cache_control,"no-store");r.set("X-Content-Type-Options","nosniff");r.set("X-Frame-Options","DENY");r.set("Referrer-Policy","no-referrer");r.set("Content-Security-Policy","default-src 'self'; img-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");r.keep_alive(false);r.body()=std::move(body);r.prepare_payload();return r;}
static Response problem(unsigned ver,http::status s,const std::string& msg){return respond(s,ver,"{\"error\":\""+esc(msg)+"\"}");}
static bool valid_id(std::string s){return s.size()==24&&std::all_of(s.begin(),s.end(),[](char c){return(c>='a'&&c<='f')||(c>='0'&&c<='9');});}

static std::string extract_token(const http::request<http::string_body>& req){
 if(auto it=req.find(http::field::authorization);it!=req.end()){
  std::string val(it->value());
  if(val.starts_with("Bearer "))return val.substr(7);
  if(val.starts_with("bearer "))return val.substr(7);
 }
 if(auto it=req.find("X-Rizoma-Token");it!=req.end()){
  return std::string(it->value());
 }
 if(auto it=req.find(http::field::cookie);it!=req.end()){
  std::string cookies(it->value());
  std::string prefix="morfo_session=";
  auto pos=cookies.find(prefix);
  if(pos!=std::string::npos){
   auto start=pos+prefix.size();
   auto end=cookies.find(';',start);
   return cookies.substr(start,end==std::string::npos?std::string::npos:end-start);
  }
 }
 return "";
}
struct UserInfo{std::string id;std::string email;std::string name;std::string role;};
static std::optional<UserInfo> authenticate(const App& app,const http::request<http::string_body>& req){
 std::string token=extract_token(req);
 if(token.empty())return std::nullopt;
 Db d(app.root/"morfo.sqlite3");
 auto user=app.auth_service.validate_session(d.p,token);
 if(!user)return std::nullopt;
 return UserInfo{user->id,user->email,user->name,user->role};
}


// MORFO profile service. Protected by the existing image ownership/authentication gate.
// Distinct from legacy region suggestions: masks are immutable derivatives of explicit runs.
static std::string morfo_profile_list(const App& app,const std::string& image_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 owned.bind(1,image_id);owned.bind(2,user_id);
 if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");
 Stmt st(db.p,"SELECT id,candidate_pixels,components,border_abstentions,status,created_at FROM morfo_profile_runs WHERE image_id=? AND user_id=? AND profile='roots-exp003' AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC");
 st.bind(1,image_id);st.bind(2,user_id);
 std::string result="{\"data\":[";bool first=true;
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)result+=',';first=false;
  result+="{\"id\":"+q(col(st.p,0))+",\"candidate_pixels\":"+std::to_string(sqlite3_column_int64(st.p,1))+
   ",\"components\":"+std::to_string(sqlite3_column_int64(st.p,2))+
   ",\"border_abstentions\":"+std::to_string(sqlite3_column_int64(st.p,3))+
   ",\"status\":"+q(col(st.p,4))+",\"created_at\":"+q(col(st.p,5))+"}";
 }
 return result+"]}";
}
static std::string morfo_delete_profile(const App& app,const std::string& image_id,const std::string& run_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");const auto now=utc_offset(0);sql(db.p,"BEGIN IMMEDIATE");try{Stmt update(db.p,"UPDATE morfo_profile_runs SET deleted_at=? WHERE id=? AND image_id=? AND user_id=? AND profile='roots-exp003' AND deleted_at IS NULL");update.bind(1,now);update.bind(2,run_id);update.bind(3,image_id);update.bind(4,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Profile run not found");Stmt metrics(db.p,"UPDATE morfo_root_metric_runs SET deleted_at=? WHERE profile_run_id=? AND user_id=? AND deleted_at IS NULL");metrics.bind(1,now);metrics.bind(2,run_id);metrics.bind(3,user_id);if(sqlite3_step(metrics.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));Stmt event(db.p,"INSERT INTO events(image_id,event,at,detail) VALUES(?,'PROFILE_RUN_DELETED',?,?)");event.bind(1,image_id);event.bind(2,now);event.bind(3,"profile=roots-exp003 | run_id="+run_id);if(sqlite3_step(event.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));sql(db.p,"COMMIT");}catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}return "{\"data\":{\"id\":"+q(run_id)+",\"image_id\":"+q(image_id)+",\"deleted\":true,\"deleted_at\":"+q(now)+"}}";
}
static std::string morfo_start_profile(const App& app,const std::string& image_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt image(db.p,"SELECT original_path,sha256,width,height FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 image.bind(1,image_id);image.bind(2,user_id);
 if(sqlite3_step(image.p)!=SQLITE_ROW)throw Failure("Image not found");
 const auto path=col(image.p,0),source_sha=col(image.p,1);
 const auto w=sqlite3_column_int64(image.p,2),h=sqlite3_column_int64(image.p,3);
 if(w<=0||h<=0||static_cast<std::uint64_t>(w)>12'000'000ULL/static_cast<std::uint64_t>(h))
  throw Failure("EXP-003 requires <= 12 MP full-resolution image; tiling is not validated");
 const std::string run_id=rand_id(),now=utc_offset(0);
 const fs::path relative=fs::path("derived")/image_id/"profiles"/"roots-exp003"/run_id;
 const fs::path output=app.root/relative;
 try {
  const auto result=morfo::profiles::run_exp003(app.root/path,output);
  if(result.original_sha256!=source_sha)throw Failure("Original hash changed since ingest");
  Stmt insert(db.p,"INSERT INTO morfo_profile_runs(id,image_id,user_id,profile,version,original_sha256,mask_sha256,width,height,candidate_pixels,components,border_abstentions,relative_dir,status,created_at) VALUES(?,?,?,'roots-exp003','exp003-original-0.1.0',?,?,?,?,?,?,?,?,'NOT_VALIDATED',?)");
  insert.bind(1,run_id);insert.bind(2,image_id);insert.bind(3,user_id);
  insert.bind(4,result.original_sha256);insert.bind(5,result.mask_sha256);
  sqlite3_bind_int64(insert.p,6,static_cast<sqlite3_int64>(result.width));
  sqlite3_bind_int64(insert.p,7,static_cast<sqlite3_int64>(result.height));
  sqlite3_bind_int64(insert.p,8,static_cast<sqlite3_int64>(result.candidate_pixels));
  sqlite3_bind_int64(insert.p,9,static_cast<sqlite3_int64>(result.components));
  sqlite3_bind_int64(insert.p,10,static_cast<sqlite3_int64>(result.border_abstentions));
  insert.bind(11,relative.generic_string());insert.bind(12,now);
  if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure("Could not persist MORFO profile run");
  return "{\"data\":{\"id\":"+q(run_id)+",\"profile\":\"roots-exp003\",\"status\":\"COMPLETED\",\"scientific_status\":\"NOT_VALIDATED\",\"biological_identity\":\"UNKNOWN\",\"candidate_pixels\":"+std::to_string(result.candidate_pixels)+",\"components\":"+std::to_string(result.components)+",\"border_abstentions\":"+std::to_string(result.border_abstentions)+",\"mask_sha256\":"+q(result.mask_sha256)+"}}";
 }catch(...){std::error_code ignored;fs::remove_all(output,ignored);throw;}
}
static std::string morfo_profile_artifact(const App& app,const std::string& image_id,const std::string& user_id,const std::string& suffix,std::string& mime){
 const auto slash=suffix.find('/');if(slash==std::string::npos)throw Failure("Invalid profile artifact");
 const auto run_id=suffix.substr(0,slash),kind=suffix.substr(slash+1);
 if(!valid_id(run_id)||(kind!="mask"&&kind!="overlay"&&kind!="report"))throw Failure("Invalid profile artifact");
 Db db(app.root/"morfo.sqlite3");
 Stmt st(db.p,"SELECT r.relative_dir FROM morfo_profile_runs r JOIN images i ON i.id=r.image_id WHERE r.id=? AND r.image_id=? AND r.user_id=? AND r.deleted_at IS NULL AND i.user_id=? AND i.deleted_at IS NULL");
 st.bind(1,run_id);st.bind(2,image_id);st.bind(3,user_id);st.bind(4,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Profile run not found");
 const fs::path name=(kind=="report"?"report.json":kind=="mask"?"mask.png":"overlay.png");
 mime=kind=="report"?"application/json; charset=utf-8":"image/png";
 return slurp(app.root/col(st.p,0)/name);
}

static rizoma::ReferenceParams reference_params(const std::string& body){
 rizoma::ReferenceParams p;
 auto integer=[&](const char* key,int current){return json_extract_int(body,key,current);};
 auto number=[&](const char* key,double current){auto value=json_extract_optional_double(body,key);return value?*value:current;};
 p.score_threshold=integer("score_threshold",p.score_threshold);p.brightness_threshold=integer("brightness_threshold",p.brightness_threshold);
 p.min_component_area=integer("min_component_area",p.min_component_area);p.max_component_area=integer("max_component_area",p.max_component_area);
 p.min_elongation=number("min_elongation",p.min_elongation);p.top_hat_width=integer("top_hat_width",p.top_hat_width);p.top_hat_height=integer("top_hat_height",p.top_hat_height);
 p.top_hat_weight=integer("top_hat_weight",p.top_hat_weight);p.small_component_limit=integer("small_component_limit",p.small_component_limit);p.fill_factor=number("fill_factor",p.fill_factor);
 p.median_width=integer("median_width",p.median_width);p.median_height=integer("median_height",p.median_height);p.opening_width=integer("opening_width",p.opening_width);p.opening_height=integer("opening_height",p.opening_height);
 rizoma::validate(p);return p;
}
static std::string reference_params_json(const rizoma::ReferenceParams& p){std::ostringstream o;o<<"{\"schema\":\"morfo.alg.raiz.001.parameters.v0.2.0\",\"score_threshold\":"<<p.score_threshold<<",\"brightness_threshold\":"<<p.brightness_threshold<<",\"min_component_area\":"<<p.min_component_area<<",\"max_component_area\":"<<p.max_component_area<<",\"min_elongation\":"<<p.min_elongation<<",\"top_hat_width\":"<<p.top_hat_width<<",\"top_hat_height\":"<<p.top_hat_height<<",\"top_hat_weight\":"<<p.top_hat_weight<<",\"small_component_limit\":"<<p.small_component_limit<<",\"fill_factor\":"<<p.fill_factor<<",\"median_width\":"<<p.median_width<<",\"median_height\":"<<p.median_height<<",\"opening_width\":"<<p.opening_width<<",\"opening_height\":"<<p.opening_height<<'}';return o.str();}
static rizoma::Gray gray_mask_file(const fs::path& path){const auto raster=png_decode(slurp(path));rizoma::Gray mask{raster.w,raster.h,std::vector<std::uint8_t>(std::size_t(raster.w)*raster.h)};for(std::size_t i=0;i<mask.pixels.size();++i)mask.pixels[i]=(raster.rgba[i*4]||raster.rgba[i*4+1]||raster.rgba[i*4+2])?255:0;return mask;}
static std::string mask_png(const rizoma::Gray& mask){Raster r{mask.width,mask.height,std::vector<unsigned char>(mask.pixels.size()*4)};for(std::size_t i=0;i<mask.pixels.size();++i){r.rgba[i*4]=r.rgba[i*4+1]=r.rgba[i*4+2]=mask.pixels[i];r.rgba[i*4+3]=255;}return png_encode(r);}
static morfo::refinement::Selection selection_from_json(const std::string& body,int width,int height,const std::string& id){
 const auto type=json_extract_string(body,"selection_type");
 if(type=="rectangle")return morfo::refinement::rectangle(id,width,height,json_extract_int(body,"x"),json_extract_int(body,"y"),json_extract_int(body,"width"),json_extract_int(body,"height"));
 const auto coords=json_extract_number_array(body,"points");if(coords.size()<2||coords.size()%2)throw Failure("Selection points must be flat x,y pairs");std::vector<std::pair<double,double>> points;for(std::size_t i=0;i<coords.size();i+=2)points.emplace_back(coords[i],coords[i+1]);
 if(type=="polygon")return morfo::refinement::polygon(id,width,height,points);
 if(type=="brush")return morfo::refinement::brush(id,width,height,points,json_extract_optional_double(body,"brush_diameter").value_or(10));
 throw Failure("Invalid selection type");
}
static std::string refinement_parameters_contract(){const auto defaults=reference_params_json({});return "{\"data\":{\"algorithm\":\"MORFO-ALG-RAIZ-001\",\"version\":\"0.2.0\",\"defaults\":"+defaults+",\"constraints\":{\"thresholds\":\"0..255\",\"areas\":\"min >= 1; max >= min\",\"top_hat\":\"odd square; weight 0..16\",\"fill_factor\":\"0..1\",\"median\":\"3x3\",\"opening\":\"3x3\"}}}";}
static std::string parameter_presets(const App& app,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"SELECT id,name,version,parameters_json,created_at,updated_at FROM morfo_parameter_presets WHERE user_id=? AND profile='roots-reference-002' ORDER BY name");st.bind(1,user_id);std::ostringstream o;o<<"{\"data\":[";bool first=true;while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)o<<',';first=false;o<<"{\"id\":"<<q(col(st.p,0))<<",\"name\":"<<q(col(st.p,1))<<",\"version\":"<<q(col(st.p,2))<<",\"parameters\":"<<col(st.p,3)<<",\"created_at\":"<<q(col(st.p,4))<<",\"updated_at\":"<<q(col(st.p,5))<<'}';}return o<<"]}",o.str();}
static std::string save_parameter_preset(const App& app,const std::string& user_id,const std::string& body){const auto name=json_extract_string(body,"name");if(name.empty()||name.size()>80)throw Failure("Preset name must contain 1..80 characters");const auto parameters=reference_params_json(reference_params(body));Db db(app.root/"morfo.sqlite3");const auto now=utc_offset(0);Stmt existing(db.p,"SELECT id,created_at FROM morfo_parameter_presets WHERE user_id=? AND profile='roots-reference-002' AND name=?");existing.bind(1,user_id);existing.bind(2,name);std::string id=rand_id(),created=now;if(sqlite3_step(existing.p)==SQLITE_ROW){id=col(existing.p,0);created=col(existing.p,1);Stmt update(db.p,"UPDATE morfo_parameter_presets SET version='0.2.0',parameters_json=?,updated_at=? WHERE id=? AND user_id=?");update.bind(1,parameters);update.bind(2,now);update.bind(3,id);update.bind(4,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));}else{Stmt insert(db.p,"INSERT INTO morfo_parameter_presets(id,user_id,profile,name,version,parameters_json,created_at,updated_at) VALUES(?,?,'roots-reference-002',?,'0.2.0',?,?,?)");insert.bind(1,id);insert.bind(2,user_id);insert.bind(3,name);insert.bind(4,parameters);insert.bind(5,created);insert.bind(6,now);if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));}return "{\"data\":{\"id\":"+q(id)+",\"name\":"+q(name)+",\"version\":\"0.2.0\",\"parameters\":"+parameters+",\"created_at\":"+q(created)+",\"updated_at\":"+q(now)+"}}";}
static std::string delete_parameter_preset(const App& app,const std::string& user_id,const std::string& id){Db db(app.root/"morfo.sqlite3");Stmt remove(db.p,"DELETE FROM morfo_parameter_presets WHERE id=? AND user_id=? AND profile='roots-reference-002'");remove.bind(1,id);remove.bind(2,user_id);if(sqlite3_step(remove.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Parameter preset not found");return "{\"data\":{\"id\":"+q(id)+",\"deleted\":true}}";}
static std::string image_refinements(const App& app,const std::string& image_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");owned.bind(1,image_id);owned.bind(2,user_id);if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");Stmt st(db.p,R"SQL(SELECT s.id,s.base_profile_run_id,COALESCE(s.source_refinement_id,''),s.original_sha256,s.algorithm,s.algorithm_version,s.current_revision_id,s.status,s.created_at,s.updated_at,r.mask_sha256,r.ordinal FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id WHERE s.image_id=? AND s.user_id=? ORDER BY s.updated_at DESC,s.rowid DESC)SQL");st.bind(1,image_id);st.bind(2,user_id);std::ostringstream o;o<<"{\"data\":[";bool first=true;while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)o<<',';first=false;o<<"{\"id\":"<<q(col(st.p,0))<<",\"base_profile_run_id\":"<<q(col(st.p,1))<<",\"source_refinement_id\":"<<q(col(st.p,2))<<",\"original_sha256\":"<<q(col(st.p,3))<<",\"algorithm\":"<<q(col(st.p,4))<<",\"algorithm_version\":"<<q(col(st.p,5))<<",\"current_revision_id\":"<<q(col(st.p,6))<<",\"status\":"<<q(col(st.p,7))<<",\"created_at\":"<<q(col(st.p,8))<<",\"updated_at\":"<<q(col(st.p,9))<<",\"mask_sha256\":"<<q(col(st.p,10))<<",\"revision_ordinal\":"<<sqlite3_column_int(st.p,11)<<'}';}return o<<"]}",o.str();}
static std::string create_refinement(const App& app,const std::string& image_id,const std::string& user_id,const std::string& body){
 const auto requested_base=json_extract_string(body,"base_profile_run_id"),source_refinement=json_extract_string(body,"source_refinement_id");if(!valid_id(requested_base))throw Failure("Invalid base profile run identifier");if(!source_refinement.empty()&&!valid_id(source_refinement))throw Failure("Invalid source refinement identifier");Db db(app.root/"morfo.sqlite3");
 std::string base_id=requested_base,original_sha,mask_sha;fs::path source;
 if(!source_refinement.empty()){
  Stmt parent(db.p,R"SQL(SELECT s.base_profile_run_id,s.original_sha256,r.mask_relative_path,r.mask_sha256 FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id JOIN images i ON i.id=s.image_id WHERE s.id=? AND s.image_id=? AND s.user_id=? AND i.user_id=? AND i.deleted_at IS NULL AND s.status='CONFIRMED')SQL");parent.bind(1,source_refinement);parent.bind(2,image_id);parent.bind(3,user_id);parent.bind(4,user_id);if(sqlite3_step(parent.p)!=SQLITE_ROW)throw Failure("Source refinement not found");base_id=col(parent.p,0);if(base_id!=requested_base)throw Failure("Source refinement belongs to another profile run");original_sha=col(parent.p,1);source=app.root/col(parent.p,2);mask_sha=col(parent.p,3);
 }else{
  Stmt row(db.p,R"SQL(SELECT p.relative_dir,p.mask_sha256,p.original_sha256 FROM morfo_profile_runs p JOIN images i ON i.id=p.image_id WHERE p.id=? AND p.image_id=? AND p.user_id=? AND i.user_id=? AND i.deleted_at IS NULL)SQL");row.bind(1,base_id);row.bind(2,image_id);row.bind(3,user_id);row.bind(4,user_id);if(sqlite3_step(row.p)!=SQLITE_ROW)throw Failure("Profile run not found");source=app.root/col(row.p,0)/"mask.png";mask_sha=col(row.p,1);original_sha=col(row.p,2);
 }
 const std::string session_id=rand_id(),revision_id=rand_id(),now=utc_offset(0);const fs::path relative=fs::path("derived")/image_id/"refinements"/session_id;fs::create_directories(app.root/relative);fs::copy_file(source,app.root/relative/"revision-0.png",fs::copy_options::overwrite_existing);
 sql(db.p,"BEGIN IMMEDIATE");try{Stmt session(db.p,"INSERT INTO morfo_refinement_sessions(id,image_id,user_id,base_profile_run_id,source_refinement_id,original_sha256,algorithm,algorithm_version,current_revision_id,redo_revision_id,status,created_at,updated_at) VALUES(?,?,?,?,?,?,'MORFO-ALG-RAIZ-001','0.2.0',?,NULL,'DRAFT',?,?)");session.bind(1,session_id);session.bind(2,image_id);session.bind(3,user_id);session.bind(4,base_id);if(source_refinement.empty())sqlite3_bind_null(session.p,5);else session.bind(5,source_refinement);session.bind(6,original_sha);session.bind(7,revision_id);session.bind(8,now);session.bind(9,now);if(sqlite3_step(session.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));Stmt revision(db.p,"INSERT INTO morfo_refinement_revisions(id,session_id,previous_revision_id,selection_id,ordinal,intervention_type,parameters_json,mask_relative_path,mask_sha256,added_pixels,removed_pixels,created_at) VALUES(?,?,NULL,NULL,0,'BASE','{}',?,?,0,0,?)");revision.bind(1,revision_id);revision.bind(2,session_id);revision.bind(3,(relative/"revision-0.png").generic_string());revision.bind(4,mask_sha);revision.bind(5,now);if(sqlite3_step(revision.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));sql(db.p,"COMMIT");}catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);std::error_code ec;fs::remove_all(app.root/relative,ec);throw;}
 return "{\"data\":{\"id\":"+q(session_id)+",\"image_id\":"+q(image_id)+",\"base_profile_run_id\":"+q(base_id)+",\"source_refinement_id\":"+(source_refinement.empty()?"null":q(source_refinement))+",\"current_revision_id\":"+q(revision_id)+",\"status\":\"DRAFT\",\"original_sha256\":"+q(original_sha)+"}}";
}
static std::string refinement_json(const App& app,const std::string& session_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt s(db.p,"SELECT image_id,base_profile_run_id,original_sha256,algorithm,algorithm_version,current_revision_id,COALESCE(redo_revision_id,''),status,created_at,updated_at FROM morfo_refinement_sessions WHERE id=? AND user_id=?");s.bind(1,session_id);s.bind(2,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Refinement session not found");std::ostringstream o;o<<"{\"data\":{\"id\":"<<q(session_id)<<",\"image_id\":"<<q(col(s.p,0))<<",\"base_profile_run_id\":"<<q(col(s.p,1))<<",\"original_sha256\":"<<q(col(s.p,2))<<",\"algorithm\":"<<q(col(s.p,3))<<",\"algorithm_version\":"<<q(col(s.p,4))<<",\"current_revision_id\":"<<q(col(s.p,5))<<",\"can_redo\":"<<(col(s.p,6).empty()?"false":"true")<<",\"status\":"<<q(col(s.p,7))<<",\"created_at\":"<<q(col(s.p,8))<<",\"updated_at\":"<<q(col(s.p,9))<<",\"revisions\":[";Stmt r(db.p,"SELECT r.id,COALESCE(r.previous_revision_id,''),COALESCE(r.selection_id,''),r.ordinal,r.intervention_type,r.parameters_json,r.mask_sha256,r.added_pixels,r.removed_pixels,r.created_at,COALESCE(s.type,''),COALESCE(s.geometry_json,'null') FROM morfo_refinement_revisions r LEFT JOIN morfo_refinement_selections s ON s.id=r.selection_id WHERE r.session_id=? ORDER BY r.ordinal");r.bind(1,session_id);bool first=true;while(sqlite3_step(r.p)==SQLITE_ROW){if(!first)o<<',';first=false;o<<"{\"id\":"<<q(col(r.p,0))<<",\"previous_revision_id\":"<<q(col(r.p,1))<<",\"selection_id\":"<<q(col(r.p,2))<<",\"ordinal\":"<<sqlite3_column_int(r.p,3)<<",\"type\":"<<q(col(r.p,4))<<",\"parameters\":"<<col(r.p,5)<<",\"mask_sha256\":"<<q(col(r.p,6))<<",\"added_pixels\":"<<sqlite3_column_int64(r.p,7)<<",\"removed_pixels\":"<<sqlite3_column_int64(r.p,8)<<",\"created_at\":"<<q(col(r.p,9))<<",\"selection_type\":"<<q(col(r.p,10))<<",\"geometry\":"<<col(r.p,11)<<'}';}return o<<"]}}",o.str();}
static std::string apply_refinement(const App& app,const std::string& session_id,const std::string& user_id,const std::string& body){Db db(app.root/"morfo.sqlite3");Stmt s(db.p,R"SQL(SELECT s.image_id,s.current_revision_id,r.mask_relative_path,i.original_path,i.sha256 FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id JOIN images i ON i.id=s.image_id WHERE s.id=? AND s.user_id=? AND i.user_id=? AND s.status='DRAFT')SQL");s.bind(1,session_id);s.bind(2,user_id);s.bind(3,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Refinement session not found");const auto current=gray_mask_file(app.root/col(s.p,2));const std::string selection_id=rand_id();const auto selection=selection_from_json(body,current.width,current.height,selection_id);const auto action=json_extract_string(body,"intervention_type");morfo::refinement::Kind kind;if(action=="REPROCESS")kind=morfo::refinement::Kind::Reprocess;else if(action=="INCLUDE")kind=morfo::refinement::Kind::Include;else if(action=="EXCLUDE")kind=morfo::refinement::Kind::Exclude;else throw Failure("Invalid intervention type");rizoma::Gray proposed;std::string parameters="{}";if(kind==morfo::refinement::Kind::Reprocess){const auto p=reference_params(body);parameters=reference_params_json(p);proposed=rizoma::reference_mask(rizoma::read_rgb(app.root/col(s.p,3)),p);}const auto result=morfo::refinement::apply(current,selection.mask,kind,kind==morfo::refinement::Kind::Reprocess?&proposed:nullptr);std::uint64_t added=0,removed=0;for(std::size_t i=0;i<result.pixels.size();++i){if(!current.pixels[i]&&result.pixels[i])++added;if(current.pixels[i]&&!result.pixels[i])++removed;}Stmt ord(db.p,"SELECT COALESCE(MAX(ordinal),0)+1 FROM morfo_refinement_revisions WHERE session_id=?");ord.bind(1,session_id);sqlite3_step(ord.p);const int ordinal=sqlite3_column_int(ord.p,0);const std::string revision_id=rand_id(),now=utc_offset(0);const fs::path relative=fs::path("derived")/col(s.p,0)/"refinements"/session_id;const fs::path mask_rel=relative/("revision-"+std::to_string(ordinal)+".png"),selection_rel=relative/("selection-"+selection_id+".png");rizoma::write_png(app.root/mask_rel,result);rizoma::write_png(app.root/selection_rel,selection.mask);const auto mask_sha=hash256_file(app.root/mask_rel),selection_sha=hash256_file(app.root/selection_rel);
 sql(db.p,"BEGIN IMMEDIATE");try{Stmt sel(db.p,"INSERT INTO morfo_refinement_selections(id,session_id,type,geometry_json,mask_relative_path,mask_sha256,created_at) VALUES(?,?,?,?,?,?,?)");sel.bind(1,selection_id);sel.bind(2,session_id);sel.bind(3,selection.type);sel.bind(4,selection.geometry_json);sel.bind(5,selection_rel.generic_string());sel.bind(6,selection_sha);sel.bind(7,now);if(sqlite3_step(sel.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));Stmt rev(db.p,"INSERT INTO morfo_refinement_revisions(id,session_id,previous_revision_id,selection_id,ordinal,intervention_type,parameters_json,mask_relative_path,mask_sha256,added_pixels,removed_pixels,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");rev.bind(1,revision_id);rev.bind(2,session_id);rev.bind(3,col(s.p,1));rev.bind(4,selection_id);sqlite3_bind_int(rev.p,5,ordinal);rev.bind(6,action);rev.bind(7,parameters);rev.bind(8,mask_rel.generic_string());rev.bind(9,mask_sha);sqlite3_bind_int64(rev.p,10,added);sqlite3_bind_int64(rev.p,11,removed);rev.bind(12,now);if(sqlite3_step(rev.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));Stmt update(db.p,"UPDATE morfo_refinement_sessions SET current_revision_id=?,redo_revision_id=NULL,updated_at=? WHERE id=? AND user_id=?");update.bind(1,revision_id);update.bind(2,now);update.bind(3,session_id);update.bind(4,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));sql(db.p,"COMMIT");}catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);std::error_code ec;fs::remove(app.root/mask_rel,ec);fs::remove(app.root/selection_rel,ec);throw;}return refinement_json(app,session_id,user_id);}
static std::string preview_refinement(const App& app,const std::string& session_id,const std::string& user_id,const std::string& body){Db db(app.root/"morfo.sqlite3");Stmt s(db.p,R"SQL(SELECT r.mask_relative_path,i.original_path FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id JOIN images i ON i.id=s.image_id WHERE s.id=? AND s.user_id=? AND i.user_id=? AND s.status='DRAFT')SQL");s.bind(1,session_id);s.bind(2,user_id);s.bind(3,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Refinement session not found");const auto current=gray_mask_file(app.root/col(s.p,0));const auto selection=selection_from_json(body,current.width,current.height,"preview");const auto action=json_extract_string(body,"intervention_type");morfo::refinement::Kind kind;if(action=="REPROCESS")kind=morfo::refinement::Kind::Reprocess;else if(action=="INCLUDE")kind=morfo::refinement::Kind::Include;else if(action=="EXCLUDE")kind=morfo::refinement::Kind::Exclude;else throw Failure("Invalid intervention type");rizoma::Gray proposed;if(kind==morfo::refinement::Kind::Reprocess)proposed=rizoma::reference_mask(rizoma::read_rgb(app.root/col(s.p,1)),reference_params(body));return mask_png(morfo::refinement::apply(current,selection.mask,kind,kind==morfo::refinement::Kind::Reprocess?&proposed:nullptr));}
static std::string confirm_refinement(const App& app,const std::string& session_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt update(db.p,"UPDATE morfo_refinement_sessions SET status='CONFIRMED',redo_revision_id=NULL,updated_at=? WHERE id=? AND user_id=? AND status='DRAFT'");update.bind(1,utc_offset(0));update.bind(2,session_id);update.bind(3,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Refinement session not found");return refinement_json(app,session_id,user_id);}
static std::string navigate_refinement(const App& app,const std::string& session_id,const std::string& user_id,bool redo){Db db(app.root/"morfo.sqlite3");Stmt s(db.p,"SELECT current_revision_id,COALESCE(redo_revision_id,'') FROM morfo_refinement_sessions WHERE id=? AND user_id=? AND status='DRAFT'");s.bind(1,session_id);s.bind(2,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Refinement session not found");const auto current=col(s.p,0),redo_id=col(s.p,1);std::string next,redo_next;if(redo){if(redo_id.empty())throw Failure("Nothing to redo");next=redo_id;Stmt child(db.p,"SELECT id FROM morfo_refinement_revisions WHERE session_id=? AND previous_revision_id=? ORDER BY ordinal LIMIT 1");child.bind(1,session_id);child.bind(2,redo_id);if(sqlite3_step(child.p)==SQLITE_ROW)redo_next=col(child.p,0);}else{Stmt parent(db.p,"SELECT COALESCE(previous_revision_id,'') FROM morfo_refinement_revisions WHERE id=? AND session_id=?");parent.bind(1,current);parent.bind(2,session_id);if(sqlite3_step(parent.p)!=SQLITE_ROW||col(parent.p,0).empty())throw Failure("Nothing to undo");next=col(parent.p,0);redo_next=current;}Stmt update(db.p,"UPDATE morfo_refinement_sessions SET current_revision_id=?,redo_revision_id=?,updated_at=? WHERE id=? AND user_id=?");update.bind(1,next);if(redo_next.empty())sqlite3_bind_null(update.p,2);else update.bind(2,redo_next);update.bind(3,utc_offset(0));update.bind(4,session_id);update.bind(5,user_id);if(sqlite3_step(update.p)!=SQLITE_DONE)throw Failure(sqlite3_errmsg(db.p));return refinement_json(app,session_id,user_id);}
static std::string refinement_artifact(const App& app,const std::string& session_id,const std::string& user_id,const std::string& kind,std::string& mime){Db db(app.root/"morfo.sqlite3");Stmt s(db.p,R"SQL(SELECT r.mask_relative_path,i.original_path,COALESCE(prev.mask_relative_path,r.mask_relative_path) FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id LEFT JOIN morfo_refinement_revisions prev ON prev.id=r.previous_revision_id JOIN images i ON i.id=s.image_id WHERE s.id=? AND s.user_id=? AND i.user_id=?)SQL");s.bind(1,session_id);s.bind(2,user_id);s.bind(3,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Refinement session not found");mime="image/png";if(kind=="mask")return slurp(app.root/col(s.p,0));if(kind=="before")return slurp(app.root/col(s.p,2));if(kind=="difference"){const auto current=gray_mask_file(app.root/col(s.p,0)),before=gray_mask_file(app.root/col(s.p,2));Raster diff{current.width,current.height,std::vector<unsigned char>(current.pixels.size()*4)};for(std::size_t i=0;i<current.pixels.size();++i){auto* pixel=&diff.rgba[i*4];if(!before.pixels[i]&&current.pixels[i]){pixel[0]=0;pixel[1]=210;pixel[2]=190;}else if(before.pixels[i]&&!current.pixels[i]){pixel[0]=235;pixel[1]=75;pixel[2]=70;}else{const auto value=current.pixels[i]?110:25;pixel[0]=pixel[1]=pixel[2]=value;}pixel[3]=255;}return png_encode(diff);}if(kind=="overlay"){const auto mask=gray_mask_file(app.root/col(s.p,0));const auto image=rizoma::read_rgb(app.root/col(s.p,1));const fs::path temp=app.root/"staging"/("overlay-"+rand_id()+".png");rizoma::write_png(temp,rizoma::overlay(image,mask));auto bytes=slurp(temp);std::error_code ec;fs::remove(temp,ec);return bytes;}throw Failure("Invalid refinement artifact");}
static std::string refinement_bundle(const App& app,const std::string& session_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt s(db.p,R"SQL(SELECT i.original_path,r.mask_relative_path FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id JOIN images i ON i.id=s.image_id WHERE s.id=? AND s.user_id=? AND i.user_id=? AND i.deleted_at IS NULL AND s.status='CONFIRMED')SQL");s.bind(1,session_id);s.bind(2,user_id);s.bind(3,user_id);if(sqlite3_step(s.p)!=SQLITE_ROW)throw Failure("Confirmed refinement not found");const fs::path original=app.root/col(s.p,0);if(fs::file_size(original)>512ULL*1024*1024)throw Failure("Original exceeds 512 MiB refinement bundle limit");std::string extension=original.extension().string();if(extension.size()<2||extension.size()>12||!std::all_of(extension.begin()+1,extension.end(),[](unsigned char c){return std::isalnum(c);}))extension=".bin";std::string mime;auto metadata=refinement_json(app,session_id,user_id);metadata+='\n';return zip_store({{"original"+extension,slurp(original)},{"mask.png",slurp(app.root/col(s.p,1))},{"overlay.png",refinement_artifact(app,session_id,user_id,"overlay",mime)},{"metadata.json",metadata}});
}

static std::string metric_result_json(const morfo::root_metrics::Result& result,const std::string& id,const std::string& created_at){
 auto optional_number=[](const std::optional<double>& value){if(!value)return std::string("null");std::ostringstream out;out<<std::setprecision(15)<<*value;return out.str();};
 std::ostringstream out;
 out<<"{\"data\":{\"id\":"<<q(id)<<",\"profile_run_id\":"<<q(result.mask_run_id)
  <<",\"metrics_version\":"<<q(result.metrics_version)<<",\"scientific_status\":"<<q(result.status)
  <<",\"mask_sha256\":"<<q(result.mask_sha256)
  <<",\"nature\":"<<q(result.nature)<<",\"created_at\":"<<q(created_at)
  <<",\"width\":"<<result.width<<",\"height\":"<<result.height
  <<",\"roi_pixels\":"<<result.roi_pixels<<",\"valid_pixels\":"<<result.valid_pixels
  <<",\"candidate_pixels\":"<<result.candidate_pixels<<",\"matrix_pixels\":"<<result.matrix_pixels
  <<",\"excluded_or_abstained_pixels\":"<<result.excluded_or_abstained_pixels
  <<",\"occupancy_percent\":"<<optional_number(result.occupancy_percent)
  <<",\"projected_area_mm2\":"<<optional_number(result.projected_area_mm2)
  <<",\"projected_area_cm2\":"<<optional_number(result.projected_area_cm2)
  <<",\"skeleton_pixels\":"<<result.skeleton_pixels<<",\"graph_edges\":"<<result.graph_edges
  <<",\"projected_length_px\":"<<result.projected_length_px
  <<",\"projected_length_mm\":"<<optional_number(result.projected_length_mm)
  <<",\"projected_length_cm\":"<<optional_number(result.projected_length_cm)
  <<",\"apparent_tips\":"<<result.apparent_tips<<",\"truncated_tips\":"<<result.truncated_tips
  <<",\"isolated_skeleton_pixels\":"<<result.isolated_skeleton_pixels<<",\"apparent_branches\":"<<result.apparent_branches
  <<",\"branch_incident_arms\":[";
 for(std::size_t index=0;index<result.branch_incident_arms.size();++index){if(index)out<<',';out<<result.branch_incident_arms[index];}
 out<<"],\"mean_diameter\":"<<optional_number(result.mean_diameter)<<",\"min_diameter\":"<<optional_number(result.min_diameter)
  <<",\"max_diameter\":"<<optional_number(result.max_diameter)<<",\"measurable_diameter_length_fraction\":"<<result.measurable_diameter_length_fraction
  <<",\"diameter_unit\":"<<q(result.diameter_unit)<<",\"diameter_classes\":[";
 for(std::size_t index=0;index<result.diameter_classes.size();++index){if(index)out<<',';const auto& item=result.diameter_classes[index];out<<"{\"lower_bound\":"<<item.lower_bound<<",\"upper_bound\":"<<optional_number(item.upper_bound)<<",\"length\":"<<item.length<<",\"percent\":"<<item.percent<<'}';}
 out<<"],\"scale_x_mm_per_pixel\":"<<optional_number(result.calibration.scale_x_mm_per_pixel)
  <<",\"scale_y_mm_per_pixel\":"<<optional_number(result.calibration.scale_y_mm_per_pixel)
  <<",\"components\":[";
 bool first=true;for(const auto& component:result.components){if(!first)out<<',';first=false;
  out<<"{\"id\":"<<component.id<<",\"area_px\":"<<component.area_px<<",\"area_mm2\":"<<optional_number(component.area_mm2)
   <<",\"centroid_x_px\":"<<component.centroid_x_px<<",\"centroid_y_px\":"<<component.centroid_y_px
   <<",\"bounding_box\":{\"min_x\":"<<component.bounding_box.min_x<<",\"min_y\":"<<component.bounding_box.min_y
   <<",\"max_x\":"<<component.bounding_box.max_x<<",\"max_y\":"<<component.bounding_box.max_y<<'}'
   <<",\"touches_image_boundary\":"<<(component.touches_image_boundary?"true":"false")
   <<",\"touches_roi_boundary\":"<<(component.touches_roi_boundary?"true":"false")
   <<",\"touches_abstained_area\":"<<(component.touches_abstained_area?"true":"false")
   <<",\"skeleton_pixels\":"<<component.skeleton_pixels<<",\"graph_edges\":"<<component.graph_edges
   <<",\"projected_length_px\":"<<component.projected_length_px
   <<",\"projected_length_mm\":"<<optional_number(component.projected_length_mm)
   <<",\"projected_length_cm\":"<<optional_number(component.projected_length_cm)
   <<",\"apparent_tips\":"<<component.apparent_tips<<",\"isolated_skeleton_pixels\":"<<component.isolated_skeleton_pixels
   <<",\"apparent_branches\":"<<component.apparent_branches<<'}';
 }
 return out<<"]}}",out.str();
}

static std::string morfo_start_metrics(const App& app,const std::string& image_id,const std::string& profile_run_id,const std::string& user_id,const std::string& body,const std::string& refinement_id=""){
 Db db(app.root/"morfo.sqlite3");
 fs::path mask_path;std::string mask_sha,profile_version,effective_profile_run=profile_run_id;
 if(refinement_id.empty()){
  Stmt run(db.p,"SELECT relative_dir,mask_sha256,version FROM morfo_profile_runs WHERE id=? AND image_id=? AND user_id=? AND profile='roots-exp003' AND deleted_at IS NULL");run.bind(1,profile_run_id);run.bind(2,image_id);run.bind(3,user_id);if(sqlite3_step(run.p)!=SQLITE_ROW)throw Failure("Profile run not found");mask_path=app.root/col(run.p,0)/"mask.png";mask_sha=col(run.p,1);profile_version=col(run.p,2);
 }else{
  Stmt run(db.p,R"SQL(SELECT s.base_profile_run_id,r.mask_relative_path,r.mask_sha256,p.version FROM morfo_refinement_sessions s JOIN morfo_refinement_revisions r ON r.id=s.current_revision_id JOIN morfo_profile_runs p ON p.id=s.base_profile_run_id WHERE s.id=? AND s.image_id=? AND s.user_id=? AND s.status='CONFIRMED')SQL");run.bind(1,refinement_id);run.bind(2,image_id);run.bind(3,user_id);if(sqlite3_step(run.p)!=SQLITE_ROW)throw Failure("Confirmed refinement not found");effective_profile_run=col(run.p,0);mask_path=app.root/col(run.p,1);mask_sha=col(run.p,2);profile_version=col(run.p,3)+"+refinement-0.2.0";
 }
 const auto decoded=png_decode(slurp(mask_path));
 morfo::root_metrics::Input input;
 input.mask={static_cast<std::size_t>(decoded.w),static_cast<std::size_t>(decoded.h),{}};
 input.mask.pixels.resize(static_cast<std::size_t>(decoded.w)*decoded.h);
 for(std::size_t i=0;i<input.mask.pixels.size();++i)input.mask.pixels[i]=(decoded.rgba[i*4]!=0||decoded.rgba[i*4+1]!=0||decoded.rgba[i*4+2]!=0)?1:0;
 input.roi={input.mask.width,input.mask.height,std::vector<std::uint8_t>(input.mask.pixels.size(),1)};
 input.valid_mask=input.roi;
 constexpr std::size_t border=10;
 for(std::size_t y=0;y<input.mask.height;++y)for(std::size_t x=0;x<input.mask.width;++x)
  if(x<border||y<border||x+border>=input.mask.width||y+border>=input.mask.height)input.valid_mask.pixels[y*input.mask.width+x]=0;
 input.mask_run_id=refinement_id.empty()?profile_run_id:refinement_id;input.mask_sha256=mask_sha;input.profile_version=profile_version;
 input.profile_parameters_json="{\"valid_border_px\":10,\"connectivity\":8}";
 input.calibration.scale_x_mm_per_pixel=json_extract_optional_double(body,"scale_x");
 input.calibration.scale_y_mm_per_pixel=json_extract_optional_double(body,"scale_y");
 input.calibration.origin=json_extract_string(body,"origin");input.calibration.method=json_extract_string(body,"method");
 input.calibration.date=json_extract_string(body,"date");input.calibration.version=json_extract_string(body,"version");
 input.calibration.responsible=json_extract_string(body,"responsible");
 input.diameter_bin_upper_bounds=json_extract_number_array(body,"diameter_bins");
 const auto result=morfo::root_metrics::compute_gate_001a(input);
 const std::string id=rand_id(),created=utc_offset(0);
 const std::string response=metric_result_json(result,id,created);
 std::ostringstream calibration;calibration<<"{\"scale_x\":"<<(input.calibration.scale_x_mm_per_pixel?std::to_string(*input.calibration.scale_x_mm_per_pixel):"null")
  <<",\"scale_y\":"<<(input.calibration.scale_y_mm_per_pixel?std::to_string(*input.calibration.scale_y_mm_per_pixel):"null")<<",\"method\":"<<q(input.calibration.method)<<'}';
 sql(db.p,"BEGIN IMMEDIATE");try{
  Stmt insert(db.p,"INSERT INTO morfo_root_metric_runs(id,profile_run_id,refinement_id,image_id,user_id,metrics_version,mask_sha256,calibration_json,result_json,status,notes,created_at) VALUES(?,?,?,?,?,?,?,?,?,'NOT_VALIDATED','',?)");
  insert.bind(1,id);insert.bind(2,effective_profile_run);if(refinement_id.empty())sqlite3_bind_null(insert.p,3);else insert.bind(3,refinement_id);insert.bind(4,image_id);insert.bind(5,user_id);insert.bind(6,result.metrics_version);insert.bind(7,mask_sha);insert.bind(8,calibration.str());insert.bind(9,response);insert.bind(10,created);
  if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure("Could not persist root metric run");
  for(const auto& component:result.components){
   Stmt item(db.p,"INSERT INTO morfo_root_metric_components(metric_run_id,component_id,area_px,area_mm2,centroid_x,centroid_y,min_x,min_y,max_x,max_y,touches_image_boundary,touches_roi_boundary,touches_abstained_area,skeleton_pixels,graph_edges,projected_length_px,projected_length_mm,projected_length_cm,apparent_tips,isolated_skeleton_pixels,apparent_branches) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
   item.bind(1,id);sqlite3_bind_int64(item.p,2,component.id);sqlite3_bind_int64(item.p,3,component.area_px);
   if(component.area_mm2)sqlite3_bind_double(item.p,4,*component.area_mm2);else sqlite3_bind_null(item.p,4);
   sqlite3_bind_double(item.p,5,component.centroid_x_px);sqlite3_bind_double(item.p,6,component.centroid_y_px);
   sqlite3_bind_int64(item.p,7,component.bounding_box.min_x);sqlite3_bind_int64(item.p,8,component.bounding_box.min_y);sqlite3_bind_int64(item.p,9,component.bounding_box.max_x);sqlite3_bind_int64(item.p,10,component.bounding_box.max_y);
   sqlite3_bind_int(item.p,11,component.touches_image_boundary);sqlite3_bind_int(item.p,12,component.touches_roi_boundary);sqlite3_bind_int(item.p,13,component.touches_abstained_area);
   sqlite3_bind_int64(item.p,14,component.skeleton_pixels);sqlite3_bind_int64(item.p,15,component.graph_edges);sqlite3_bind_double(item.p,16,component.projected_length_px);
   if(component.projected_length_mm)sqlite3_bind_double(item.p,17,*component.projected_length_mm);else sqlite3_bind_null(item.p,17);
   if(component.projected_length_cm)sqlite3_bind_double(item.p,18,*component.projected_length_cm);else sqlite3_bind_null(item.p,18);
   sqlite3_bind_int64(item.p,19,component.apparent_tips);sqlite3_bind_int64(item.p,20,component.isolated_skeleton_pixels);sqlite3_bind_int64(item.p,21,component.apparent_branches);
   if(sqlite3_step(item.p)!=SQLITE_DONE)throw Failure("Could not persist root metric component");
  }
  sql(db.p,"COMMIT");
 }catch(...){sqlite3_exec(db.p,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
 return response;
}

static std::string morfo_latest_metrics(const App& app,const std::string& image_id,const std::string& profile_run_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"SELECT result_json FROM morfo_root_metric_runs WHERE profile_run_id=? AND refinement_id IS NULL AND image_id=? AND user_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC LIMIT 1");
 st.bind(1,profile_run_id);st.bind(2,image_id);st.bind(3,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)return "{\"data\":null}";
 return col(st.p,0);
}
static std::string refinement_latest_metrics(const App& app,const std::string& refinement_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"SELECT result_json FROM morfo_root_metric_runs WHERE refinement_id=? AND user_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC LIMIT 1");st.bind(1,refinement_id);st.bind(2,user_id);if(sqlite3_step(st.p)!=SQLITE_ROW)return "{\"data\":null}";return col(st.p,0);}

static std::string morfo_metrics_csv(const App& app,const std::string& image_id,const std::string& profile_run_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt run(db.p,R"SQL(SELECT id,metrics_version,mask_sha256,created_at,
  json_extract(result_json,'$.data.scientific_status'),json_extract(result_json,'$.data.nature'),
  json_extract(result_json,'$.data.candidate_pixels'),json_extract(result_json,'$.data.valid_pixels'),
  json_extract(result_json,'$.data.matrix_pixels'),json_extract(result_json,'$.data.excluded_or_abstained_pixels'),
  json_extract(result_json,'$.data.occupancy_percent'),json_extract(result_json,'$.data.projected_area_mm2'),
  json_extract(result_json,'$.data.projected_area_cm2'),json_extract(result_json,'$.data.scale_x_mm_per_pixel'),
  json_extract(result_json,'$.data.scale_y_mm_per_pixel'),json_extract(result_json,'$.data.skeleton_pixels'),
  json_extract(result_json,'$.data.graph_edges'),json_extract(result_json,'$.data.projected_length_px'),
  json_extract(result_json,'$.data.projected_length_mm'),json_extract(result_json,'$.data.projected_length_cm'),
  json_extract(result_json,'$.data.mean_diameter'),json_extract(result_json,'$.data.min_diameter'),
  json_extract(result_json,'$.data.max_diameter'),json_extract(result_json,'$.data.measurable_diameter_length_fraction'),
  json_extract(result_json,'$.data.diameter_unit')
 FROM morfo_root_metric_runs WHERE profile_run_id=? AND refinement_id IS NULL AND image_id=? AND user_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC LIMIT 1)SQL");
 run.bind(1,profile_run_id);run.bind(2,image_id);run.bind(3,user_id);if(sqlite3_step(run.p)!=SQLITE_ROW)throw Failure("Root metric run not found");
 auto value=[&](int column){return sqlite3_column_type(run.p,column)==SQLITE_NULL?std::string(""):col(run.p,column);};
 std::ostringstream out;out<<"# MORFO-ROOT-METRICS-001C\r\n"
  <<"# metric_run_id,"<<csv_cell(col(run.p,0))<<"\r\n# profile_run_id,"<<csv_cell(profile_run_id)
  <<"\r\n# image_id,"<<csv_cell(image_id)<<"\r\n# metrics_version,"<<csv_cell(col(run.p,1))
  <<"\r\n# mask_sha256,"<<csv_cell(col(run.p,2))<<"\r\n# created_at,"<<csv_cell(col(run.p,3))
  <<"\r\n# scientific_status,"<<csv_cell(value(4))<<"\r\n# nature,"<<csv_cell(value(5))
  <<"\r\n# candidate_pixels,"<<value(6)<<"\r\n# valid_pixels,"<<value(7)<<"\r\n# matrix_pixels,"<<value(8)
  <<"\r\n# excluded_or_abstained_pixels,"<<value(9)<<"\r\n# occupancy_percent,"<<value(10)
  <<"\r\n# projected_area_mm2,"<<value(11)<<"\r\n# projected_area_cm2,"<<value(12)
  <<"\r\n# scale_x_mm_per_pixel,"<<value(13)<<"\r\n# scale_y_mm_per_pixel,"<<value(14)
  <<"\r\n# skeleton_pixels,"<<value(15)<<"\r\n# graph_edges,"<<value(16)<<"\r\n# projected_length_px,"<<value(17)
  <<"\r\n# projected_length_mm,"<<value(18)<<"\r\n# projected_length_cm,"<<value(19)
  <<"\r\n# mean_diameter,"<<value(20)<<"\r\n# min_diameter,"<<value(21)<<"\r\n# max_diameter,"<<value(22)
  <<"\r\n# measurable_diameter_length_fraction,"<<value(23)<<"\r\n# diameter_unit,"<<value(24)<<"\r\n"
  <<"component_id,area_px,area_mm2,centroid_x_px,centroid_y_px,min_x,min_y,max_x,max_y,touches_image_boundary,touches_roi_boundary,touches_abstained_area,skeleton_pixels,graph_edges,projected_length_px,projected_length_mm,projected_length_cm\r\n";
 Stmt components(db.p,"SELECT component_id,area_px,area_mm2,centroid_x,centroid_y,min_x,min_y,max_x,max_y,touches_image_boundary,touches_roi_boundary,touches_abstained_area,skeleton_pixels,graph_edges,projected_length_px,projected_length_mm,projected_length_cm FROM morfo_root_metric_components WHERE metric_run_id=? ORDER BY component_id");components.bind(1,col(run.p,0));
 while(sqlite3_step(components.p)==SQLITE_ROW){for(int column=0;column<17;++column){if(column)out<<',';if(sqlite3_column_type(components.p,column)!=SQLITE_NULL)out<<col(components.p,column);}out<<"\r\n";}
 out<<"\r\ndiameter_lower_bound,diameter_upper_bound,length,percent\r\n";Stmt classes(db.p,"SELECT json_extract(value,'$.lower_bound'),json_extract(value,'$.upper_bound'),json_extract(value,'$.length'),json_extract(value,'$.percent') FROM json_each((SELECT result_json FROM morfo_root_metric_runs WHERE id=?),'$.data.diameter_classes')");classes.bind(1,col(run.p,0));while(sqlite3_step(classes.p)==SQLITE_ROW){for(int column=0;column<4;++column){if(column)out<<',';if(sqlite3_column_type(classes.p,column)!=SQLITE_NULL)out<<col(classes.p,column);}out<<"\r\n";}
 return out.str();
}

static std::string morfo_metric_runs_json(const App& app,const std::string& image_id,const std::string& profile_run_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,R"SQL(SELECT id,metrics_version,status,notes,created_at,
 json_extract(result_json,'$.data.candidate_pixels'),json_extract(result_json,'$.data.occupancy_percent'),
 json_extract(result_json,'$.data.projected_area_mm2'),json_array_length(json_extract(result_json,'$.data.components'))
 FROM morfo_root_metric_runs WHERE profile_run_id=? AND refinement_id IS NULL AND image_id=? AND user_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC)SQL");
 st.bind(1,profile_run_id);st.bind(2,image_id);st.bind(3,user_id);std::ostringstream out;out<<"{\"data\":[";bool first=true;
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)out<<',';first=false;out<<"{\"id\":"<<q(col(st.p,0))<<",\"metrics_version\":"<<q(col(st.p,1))<<",\"status\":"<<q(col(st.p,2))<<",\"notes\":"<<q(col(st.p,3))<<",\"created_at\":"<<q(col(st.p,4))<<",\"candidate_pixels\":"<<sqlite3_column_int64(st.p,5)<<",\"occupancy_percent\":"<<(sqlite3_column_type(st.p,6)==SQLITE_NULL?"null":col(st.p,6))<<",\"projected_area_mm2\":"<<(sqlite3_column_type(st.p,7)==SQLITE_NULL?"null":col(st.p,7))<<",\"components\":"<<sqlite3_column_int64(st.p,8)<<'}';}
 return out<<"]}",out.str();
}

static std::string refinement_metric_runs_json(const App& app,const std::string& refinement_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,R"SQL(SELECT id,metrics_version,status,notes,created_at,
 json_extract(result_json,'$.data.candidate_pixels'),json_extract(result_json,'$.data.occupancy_percent'),
 json_extract(result_json,'$.data.projected_area_mm2'),json_array_length(json_extract(result_json,'$.data.components'))
 FROM morfo_root_metric_runs WHERE refinement_id=? AND user_id=? AND deleted_at IS NULL ORDER BY created_at DESC,rowid DESC)SQL");
 st.bind(1,refinement_id);st.bind(2,user_id);std::ostringstream out;out<<"{\"data\":[";bool first=true;
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)out<<',';first=false;out<<"{\"id\":"<<q(col(st.p,0))<<",\"metrics_version\":"<<q(col(st.p,1))<<",\"status\":"<<q(col(st.p,2))<<",\"notes\":"<<q(col(st.p,3))<<",\"created_at\":"<<q(col(st.p,4))<<",\"candidate_pixels\":"<<sqlite3_column_int64(st.p,5)<<",\"occupancy_percent\":"<<(sqlite3_column_type(st.p,6)==SQLITE_NULL?"null":col(st.p,6))<<",\"projected_area_mm2\":"<<(sqlite3_column_type(st.p,7)==SQLITE_NULL?"null":col(st.p,7))<<",\"components\":"<<sqlite3_column_int64(st.p,8)<<'}';}
 return out<<"]}",out.str();
}

static std::string all_morfo_metric_runs_json(const App& app,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,R"SQL(SELECT m.id,m.profile_run_id,m.image_id,i.filename,i.sample_id,m.metrics_version,m.status,m.notes,m.created_at,
 json_extract(m.result_json,'$.data.projected_area_mm2'),json_extract(m.result_json,'$.data.candidate_pixels'),
 json_extract(m.result_json,'$.data.projected_length_mm'),json_extract(m.result_json,'$.data.projected_length_px'),
 json_extract(m.result_json,'$.data.mean_diameter'),json_extract(m.result_json,'$.data.diameter_unit'),
 json_extract(m.result_json,'$.data.apparent_tips'),json_extract(m.result_json,'$.data.apparent_branches'),
 json_extract(m.result_json,'$.data.occupancy_percent')
 FROM morfo_root_metric_runs m JOIN images i ON i.id=m.image_id
 WHERE m.user_id=? AND m.deleted_at IS NULL AND i.deleted_at IS NULL ORDER BY m.created_at DESC,m.rowid DESC)SQL");
 st.bind(1,user_id);std::ostringstream out;out<<"{\"data\":[";bool first=true;auto nullable=[&](int column){return sqlite3_column_type(st.p,column)==SQLITE_NULL?std::string("null"):col(st.p,column);};
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)out<<',';first=false;out<<"{\"id\":"<<q(col(st.p,0))<<",\"profile_run_id\":"<<q(col(st.p,1))<<",\"image_id\":"<<q(col(st.p,2))<<",\"filename\":"<<q(col(st.p,3))<<",\"sample_id\":"<<q(col(st.p,4))<<",\"metrics_version\":"<<q(col(st.p,5))<<",\"status\":"<<q(col(st.p,6))<<",\"notes\":"<<q(col(st.p,7))<<",\"created_at\":"<<q(col(st.p,8))<<",\"projected_area_mm2\":"<<nullable(9)<<",\"candidate_pixels\":"<<sqlite3_column_int64(st.p,10)<<",\"projected_length_mm\":"<<nullable(11)<<",\"projected_length_px\":"<<nullable(12)<<",\"mean_diameter\":"<<nullable(13)<<",\"diameter_unit\":"<<q(col(st.p,14))<<",\"apparent_tips\":"<<nullable(15)<<",\"apparent_branches\":"<<nullable(16)<<",\"occupancy_percent\":"<<nullable(17)<<'}';}
 return out<<"]}",out.str();
}

static std::string morfo_metric_run_json(const App& app,const std::string& metric_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"SELECT result_json,status,notes FROM morfo_root_metric_runs WHERE id=? AND user_id=? AND deleted_at IS NULL");st.bind(1,metric_id);st.bind(2,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Root metric run not found");
 std::string result=col(st.p,0);const auto end=result.rfind("}}");
 if(end==std::string::npos)throw Failure("Invalid persisted metric result");
 result.insert(end,",\"record_status\":"+q(col(st.p,1))+",\"notes\":"+q(col(st.p,2)));
 return result;
}

static std::string update_morfo_metric_run(const App& app,const std::string& metric_id,const std::string& user_id,const std::string& body){
 const std::string status=json_extract_string(body,"status"),notes=json_extract_string(body,"notes");
 if(status!="NOT_VALIDATED"&&status!="REVIEWED"&&status!="ARCHIVED")throw Failure("Invalid metric status");
 if(notes.size()>2000)throw Failure("Metric notes exceed 2000 characters");
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"UPDATE morfo_root_metric_runs SET status=?,notes=? WHERE id=? AND user_id=? AND deleted_at IS NULL");st.bind(1,status);st.bind(2,notes);st.bind(3,metric_id);st.bind(4,user_id);if(sqlite3_step(st.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Root metric run not found");return morfo_metric_run_json(app,metric_id,user_id);
}

static std::string delete_morfo_metric_run(const App& app,const std::string& metric_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"UPDATE morfo_root_metric_runs SET deleted_at=? WHERE id=? AND user_id=? AND deleted_at IS NULL");st.bind(1,utc_offset(0));st.bind(2,metric_id);st.bind(3,user_id);if(sqlite3_step(st.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Root metric run not found");return "{\"status\":\"deleted\",\"id\":"+q(metric_id)+"}";
}

static std::string calibration_row_json(sqlite3_stmt* st){std::ostringstream out;out<<"{\"id\":"<<q(col(st,0))<<",\"image_id\":"<<q(col(st,1))<<",\"source_type\":"<<q(col(st,2))<<",\"scale_x_mm_per_pixel\":"<<sqlite3_column_double(st,3)<<",\"scale_y_mm_per_pixel\":"<<sqlite3_column_double(st,4)<<",\"source_detail\":"<<q(col(st,5))<<",\"method\":"<<q(col(st,6))<<",\"version\":"<<q(col(st,7))<<",\"responsible\":"<<q(col(st,8))<<",\"verified\":"<<(sqlite3_column_int(st,9)?"true":"false")<<",\"notes\":"<<q(col(st,10))<<",\"created_at\":"<<q(col(st,11))<<",\"updated_at\":"<<q(col(st,12))<<'}';return out.str();}
static bool json_extract_bool(const std::string& body,const std::string& key){const std::string needle="\""+key+"\"";auto p=body.find(needle);if(p==std::string::npos)return false;p=body.find(':',p+needle.size());if(p==std::string::npos)return false;++p;while(p<body.size()&&std::isspace(static_cast<unsigned char>(body[p])))++p;return body.compare(p,4,"true")==0;}

static std::string image_calibrations_json(const App& app,const std::string& image_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");owned.bind(1,image_id);owned.bind(2,user_id);if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");Stmt st(db.p,"SELECT id,image_id,source_type,scale_x_mm_per_pixel,scale_y_mm_per_pixel,source_detail,method,version,responsible,verified,notes,created_at,updated_at FROM morfo_image_calibrations WHERE image_id=? AND user_id=? AND deleted_at IS NULL ORDER BY verified DESC,created_at DESC,rowid DESC");st.bind(1,image_id);st.bind(2,user_id);std::string out="{\"data\":[";bool first=true;while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)out+=',';first=false;out+=calibration_row_json(st.p);}return out+"]}";}

static std::string create_image_calibration(const App& app,const std::string& image_id,const std::string& user_id,const std::string& body){const auto x=json_extract_optional_double(body,"scale_x"),y=json_extract_optional_double(body,"scale_y");if(!x||!y||*x<=0||*y<=0)throw Failure("Calibration requires positive X and Y scales");const std::string source=json_extract_string(body,"source_type");if(source!="MANUAL"&&source!="IMAGE_REFERENCE"&&source!="EQUIPMENT_METADATA")throw Failure("Invalid calibration source");const bool verified=json_extract_bool(body,"verified");if(source=="EQUIPMENT_METADATA"&&verified&&json_extract_string(body,"responsible").empty())throw Failure("Verified metadata calibration requires responsible person");Db db(app.root/"morfo.sqlite3");Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");owned.bind(1,image_id);owned.bind(2,user_id);if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");const std::string id=rand_id(),now=utc_offset(0);Stmt insert(db.p,"INSERT INTO morfo_image_calibrations(id,image_id,user_id,source_type,scale_x_mm_per_pixel,scale_y_mm_per_pixel,source_detail,method,version,responsible,verified,notes,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");insert.bind(1,id);insert.bind(2,image_id);insert.bind(3,user_id);insert.bind(4,source);sqlite3_bind_double(insert.p,5,*x);sqlite3_bind_double(insert.p,6,*y);insert.bind(7,json_extract_string(body,"source_detail"));insert.bind(8,json_extract_string(body,"method"));insert.bind(9,json_extract_string(body,"version"));insert.bind(10,json_extract_string(body,"responsible"));sqlite3_bind_int(insert.p,11,verified);insert.bind(12,json_extract_string(body,"notes"));insert.bind(13,now);insert.bind(14,now);if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure("Could not persist calibration");return image_calibrations_json(app,image_id,user_id);}

static std::string update_image_calibration(const App& app,const std::string& calibration_id,const std::string& user_id,const std::string& body){const auto x=json_extract_optional_double(body,"scale_x"),y=json_extract_optional_double(body,"scale_y");if(!x||!y||*x<=0||*y<=0)throw Failure("Calibration requires positive X and Y scales");const std::string source=json_extract_string(body,"source_type"),responsible=json_extract_string(body,"responsible");if(source!="MANUAL"&&source!="IMAGE_REFERENCE"&&source!="EQUIPMENT_METADATA")throw Failure("Invalid calibration source");const bool verified=json_extract_bool(body,"verified");if(source=="EQUIPMENT_METADATA"&&verified&&responsible.empty())throw Failure("Verified metadata calibration requires responsible person");Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"UPDATE morfo_image_calibrations SET source_type=?,scale_x_mm_per_pixel=?,scale_y_mm_per_pixel=?,source_detail=?,method=?,version=?,responsible=?,verified=?,notes=?,updated_at=? WHERE id=? AND user_id=? AND deleted_at IS NULL");st.bind(1,source);sqlite3_bind_double(st.p,2,*x);sqlite3_bind_double(st.p,3,*y);st.bind(4,json_extract_string(body,"source_detail"));st.bind(5,json_extract_string(body,"method"));st.bind(6,json_extract_string(body,"version"));st.bind(7,responsible);sqlite3_bind_int(st.p,8,verified);st.bind(9,json_extract_string(body,"notes"));st.bind(10,utc_offset(0));st.bind(11,calibration_id);st.bind(12,user_id);if(sqlite3_step(st.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Calibration not found");Stmt read(db.p,"SELECT id,image_id,source_type,scale_x_mm_per_pixel,scale_y_mm_per_pixel,source_detail,method,version,responsible,verified,notes,created_at,updated_at FROM morfo_image_calibrations WHERE id=?");read.bind(1,calibration_id);sqlite3_step(read.p);return "{\"data\":"+calibration_row_json(read.p)+"}";}

static std::string delete_image_calibration(const App& app,const std::string& calibration_id,const std::string& user_id){Db db(app.root/"morfo.sqlite3");Stmt st(db.p,"UPDATE morfo_image_calibrations SET deleted_at=?,updated_at=? WHERE id=? AND user_id=? AND deleted_at IS NULL");const auto now=utc_offset(0);st.bind(1,now);st.bind(2,now);st.bind(3,calibration_id);st.bind(4,user_id);if(sqlite3_step(st.p)!=SQLITE_DONE||sqlite3_changes(db.p)!=1)throw Failure("Calibration not found");return "{\"status\":\"deleted\",\"id\":"+q(calibration_id)+"}";}

static Response handle(const App& app,const http::request<http::string_body>& req){
 unsigned v=req.version();std::string target(req.target());auto p=target.substr(0,target.find('?'));auto args=params(target);
 if(req.method()==http::verb::get&&p=="/api/v1/health")return respond(http::status::ok,v,"{\"status\":\"ok\",\"name\":\"RIZOMA\",\"version\":\"0.2.0\",\"scope\":\"local-image-intake\"}");

 // Auth endpoints
 if(req.method()==http::verb::get&&p=="/api/v1/auth/me"){
  auto user=authenticate(app,req);
  if(!user)return respond(http::status::ok,v,"{\"authenticated\":false}");
  std::ostringstream o;
  o<<"{\"authenticated\":true,\"user\":{\"id\":"<<q(user->id)<<",\"email\":"<<q(user->email)<<",\"name\":"<<q(user->name)<<",\"role\":"<<q(user->role)<<"}}";
  return respond(http::status::ok,v,o.str());
 }
 if(req.method()==http::verb::get&&p=="/api/v1/auth/providers"){
  std::ostringstream o;
  o<<"{\"email_otp\":true,\"google_oauth\":false,\"smtp_from\":"<<q(app.auth_service.config().smtp_from)<<"}";
  return respond(http::status::ok,v,o.str());
 }
 if(req.method()==http::verb::post&&p=="/api/v1/auth/request-code"){
  std::string raw_email=json_extract_string(req.body(),"email");
  std::string email=normalize_email(raw_email);
  std::string remote_ip;
  if(auto it=req.find("X-Forwarded-For");it!=req.end()){
   remote_ip=std::string(it->value());
   auto c=remote_ip.find(',');if(c!=std::string::npos)remote_ip=remote_ip.substr(0,c);
  }else if(auto it2=req.find("X-Client-IP");it2!=req.end()){
   remote_ip=std::string(it2->value());
  }
  Db db(app.root/"morfo.sqlite3");
  auto res=app.auth_service.request_code(db.p,email,remote_ip);
  if(!res.ok){
   return problem(v,static_cast<http::status>(res.status_code),res.user_message);
  }
  std::ostringstream resp;
  resp<<"{\"status\":\"ok\",\"message\":"<<q(res.user_message);
  if(!res.dev_code.empty()){
   resp<<",\"dev_code\":"<<q(res.dev_code);
  }
  resp<<"}";
  return respond(http::status::ok,v,resp.str());
 }
 if(req.method()==http::verb::post&&p=="/api/v1/auth/verify-code"){
  std::string email=normalize_email(json_extract_string(req.body(),"email"));
  std::string code=json_extract_string(req.body(),"code");
  std::string remote_ip;
  if(auto it=req.find("X-Forwarded-For");it!=req.end()){
   remote_ip=std::string(it->value());
   auto c=remote_ip.find(',');if(c!=std::string::npos)remote_ip=remote_ip.substr(0,c);
  }else if(auto it2=req.find("X-Client-IP");it2!=req.end()){
   remote_ip=std::string(it2->value());
  }
  Db db(app.root/"morfo.sqlite3");
  auto res=app.auth_service.verify_code(db.p,email,code,remote_ip);
  if(!res.ok){
   return problem(v,static_cast<http::status>(res.status_code),res.user_message);
  }
  std::ostringstream resp;
  resp<<"{\"status\":\"ok\",\"session_token\":"<<q(res.session_token)
      <<",\"user\":{\"id\":"<<q(res.user.id)
      <<",\"email\":"<<q(res.user.email)
      <<",\"name\":"<<q(res.user.name)
      <<",\"role\":"<<q(res.user.role)<<"}}";
  auto r=respond(http::status::ok,v,resp.str());
  r.set(http::field::set_cookie,"morfo_session="+res.session_token+"; Path=/; HttpOnly; SameSite=Lax");
  return r;
 }
 if(req.method()==http::verb::post&&p=="/api/v1/auth/logout"){
  std::string token=extract_token(req);
  if(!token.empty()){
   Db db(app.root/"morfo.sqlite3");
   app.auth_service.revoke_session(db.p,token);
  }
  auto r=respond(http::status::ok,v,"{\"status\":\"ok\"}");
  r.set(http::field::set_cookie,"morfo_session=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly; SameSite=Lax");
  return r;
 }

 // Admin endpoints (strictly restricted to role == 'admin')
 if(p.starts_with("/api/v1/admin/")){
  auto user=authenticate(app,req);
  if(!user){
   return problem(v,http::status::unauthorized,"Autenticação necessária.");
  }
  if(user->role!="admin"){
   return problem(v,http::status::forbidden,"Acesso restrito a administradores.");
  }
  Db db(app.root/"morfo.sqlite3");
  if(req.method()==http::verb::get&&p=="/api/v1/admin/users"){
   auto users=app.auth_service.list_users(db.p);
   std::ostringstream resp;
   resp<<"{\"data\":[";
   bool first=true;
   for(const auto& u : users){
    if(!first)resp<<",";
    first=false;
    resp<<"{\"id\":"<<q(u.id)
        <<",\"email\":"<<q(u.email)
        <<",\"name\":"<<q(u.name)
        <<",\"role\":"<<q(u.role)
        <<",\"is_active\":"<<(u.is_active?"true":"false")
        <<",\"created_at\":"<<q(u.created_at)
        <<",\"last_login_at\":"<<q(u.last_login_at)<<"}";
   }
   resp<<"]}";
   return respond(http::status::ok,v,resp.str());
  }
  if(req.method()==http::verb::post&&p=="/api/v1/admin/users"){
   std::string email=normalize_email(json_extract_string(req.body(),"email"));
   std::string name=json_extract_string(req.body(),"name");
   std::string role=json_extract_string(req.body(),"role");
   if(email.empty()||email.find('@')==std::string::npos||email.find('.')==std::string::npos){
    return problem(v,http::status::bad_request,"E-mail inválido.");
   }
   if(name.empty())name=name_from_email(email);
   if(role!="admin"&&role!="researcher"&&role!="user"){
    role="user";
   }
   if(app.auth_service.add_user(db.p,email,name,role)){
    app.auth_service.record_audit(db.p,user->email,"","ADMIN_USER_SAVE","SUCCESS","User saved: "+email+" role: "+role);
    return respond(http::status::ok,v,"{\"status\":\"ok\",\"message\":\"Usuário salvo com sucesso.\"}");
   }else{
    return problem(v,http::status::internal_server_error,"Falha ao persistir usuário.");
   }
  }
  if(req.method()==http::verb::post&&p=="/api/v1/admin/users/status"){
   std::string email=normalize_email(json_extract_string(req.body(),"email"));
   bool active=json_extract_bool(req.body(),"is_active");
   if(email.empty()){
    return problem(v,http::status::bad_request,"E-mail não fornecido.");
   }
   if(email==user->email&&!active){
    return problem(v,http::status::bad_request,"Não é permitido desativar seu próprio usuário administrador.");
   }
   if(app.auth_service.set_user_active(db.p,email,active)){
    app.auth_service.record_audit(db.p,user->email,"","ADMIN_USER_STATUS","SUCCESS","User "+email+" active="+(active?"1":"0"));
    return respond(http::status::ok,v,"{\"status\":\"ok\",\"message\":\"Status do usuário atualizado.\"}");
   }else{
    return problem(v,http::status::internal_server_error,"Falha ao atualizar status.");
   }
  }
  if(req.method()==http::verb::get&&p=="/api/v1/admin/audit"){
   Stmt st(db.p,"SELECT id, email, ip_address, action, decision, details, occurred_at FROM auth_audit_events ORDER BY occurred_at DESC LIMIT 50;");
   std::ostringstream resp;
   resp<<"{\"data\":[";
   bool first=true;
   while(sqlite3_step(st.p)==SQLITE_ROW){
    if(!first)resp<<",";
    first=false;
    resp<<"{\"id\":"<<q(col(st.p,0))
        <<",\"email\":"<<q(col(st.p,1))
        <<",\"ip_address\":"<<q(col(st.p,2))
        <<",\"action\":"<<q(col(st.p,3))
        <<",\"decision\":"<<q(col(st.p,4))
        <<",\"details\":"<<q(col(st.p,5))
        <<",\"occurred_at\":"<<q(col(st.p,6))<<"}";
   }
   resp<<"]}";
   return respond(http::status::ok,v,resp.str());
  }
  return problem(v,http::status::not_found,"Rota administrativa não encontrada.");
 }

 if(req.method()==http::verb::get&&p=="/api/v1/profiles")return respond(http::status::ok,v,morfo::profiles::registry_json());
 if(req.method()==http::verb::get&&p=="/api/v1/refinement-parameters")return respond(http::status::ok,v,refinement_parameters_contract());

 // Protected routes
 if(p=="/api/v1/images"||p=="/api/v1/root-metrics"||p=="/api/v1/evaluation/overview"||p=="/api/v1/evaluation/protocols"||p=="/api/v1/evaluation/datasets"||p=="/api/v1/parameter-presets"||p=="/api/v1/export/manifest.csv"||p.starts_with("/api/v1/evaluation/protocols/")||p.starts_with("/api/v1/evaluation/datasets/")||p.starts_with("/api/v1/images/")||p.starts_with("/api/v1/refinements/")||p.starts_with("/api/v1/parameter-presets/")||p.starts_with("/api/v1/analysis-regions/")||p.starts_with("/api/v1/analysis-runs/")||p.starts_with("/api/v1/root-metrics/")||p.starts_with("/api/v1/calibrations/")){
  auto user=authenticate(app,req);
  if(!user){
   return problem(v,http::status::unauthorized,"Autenticação necessária. Solicite uma chave de acesso para seu e-mail.");
  }
  if(req.method()==http::verb::get&&p=="/api/v1/images")return respond(http::status::ok,v,all_images(app,user->id));
  if(req.method()==http::verb::get&&p=="/api/v1/export/manifest.csv")return respond(http::status::ok,v,manifest(app,user->id),"text/csv; charset=utf-8");
  if(req.method()==http::verb::get&&p=="/api/v1/root-metrics")return respond(http::status::ok,v,all_morfo_metric_runs_json(app,user->id));
  if(req.method()==http::verb::get&&p=="/api/v1/evaluation/overview")return respond(http::status::ok,v,evaluation_overview_json(app,user->id));
  if(req.method()==http::verb::get&&p=="/api/v1/evaluation/datasets")return respond(http::status::ok,v,evaluation_datasets_json(app,user->id));
  if(p=="/api/v1/evaluation/protocols"){
   try{if(req.method()==http::verb::get)return respond(http::status::ok,v,evaluation_protocols_json(app,user->id));if(req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,create_evaluation_protocol(app,user->id,req.body()));}}catch(const std::exception& e){return problem(v,http::status::unprocessable_entity,e.what());}
  }
  if(p.starts_with("/api/v1/evaluation/protocols/")&&req.method()==http::verb::post){const std::string rem=p.substr(std::string("/api/v1/evaluation/protocols/").size());const auto slash=rem.find('/');const std::string protocol_id=rem.substr(0,slash),action=slash==std::string::npos?"":rem.substr(slash+1);if(!valid_id(protocol_id)||(action!="freeze"&&action!="units"))return problem(v,http::status::bad_request,"Invalid evaluation protocol action");try{if(action=="units"){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,attach_profile_to_evaluation(app,user->id,protocol_id,req.body()));}return respond(http::status::ok,v,freeze_evaluation_protocol(app,user->id,protocol_id));}catch(const std::exception& e){return problem(v,std::string(e.what())=="Evaluation protocol not found"||std::string(e.what())=="Profile run not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}}
  if(p.starts_with("/api/v1/evaluation/datasets/")){const std::string rem=p.substr(std::string("/api/v1/evaluation/datasets/").size());std::vector<std::string> parts;std::istringstream stream(rem);std::string part;while(std::getline(stream,part,'/'))parts.push_back(part);if(parts.empty()||!valid_id(parts[0]))return problem(v,http::status::bad_request,"Invalid evaluation dataset action");try{if(parts.size()==2&&parts[1]=="freeze"&&req.method()==http::verb::post)return respond(http::status::ok,v,freeze_evaluation_dataset(app,user->id,parts[0]));if(parts.size()>=3&&parts[1]=="units"&&valid_id(parts[2])){if(parts.size()==3&&req.method()==http::verb::patch){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::ok,v,update_evaluation_unit(app,user->id,parts[0],parts[2],req.body()));}if(parts.size()==3&&req.method()==http::verb::delete_)return respond(http::status::ok,v,delete_evaluation_unit(app,user->id,parts[0],parts[2]));if(parts.size()==4&&parts[3]=="reference"&&req.method()==http::verb::post){if(req[http::field::content_type].find("image/png")!=0)return problem(v,http::status::unsupported_media_type,"Use image/png");const std::string filename=req.find("X-Reference-Filename")==req.end()?"reference.png":std::string(req["X-Reference-Filename"]);return respond(http::status::created,v,add_evaluation_reference(app,user->id,parts[0],parts[2],filename,req.body()));}}return problem(v,http::status::bad_request,"Invalid evaluation dataset action");}catch(const std::exception& e){const std::string message=e.what();return problem(v,message=="Evaluation dataset not found"||message=="Draft evaluation unit not found"?http::status::not_found:http::status::unprocessable_entity,message);}}
  if(p=="/api/v1/parameter-presets"){
   try{if(req.method()==http::verb::get)return respond(http::status::ok,v,parameter_presets(app,user->id));if(req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,save_parameter_preset(app,user->id,req.body()));}}catch(const std::exception& e){return problem(v,http::status::unprocessable_entity,e.what());}
  }
  if(p.starts_with("/api/v1/parameter-presets/")&&req.method()==http::verb::delete_){const auto preset_id=p.substr(std::string("/api/v1/parameter-presets/").size());if(!valid_id(preset_id))return problem(v,http::status::bad_request,"Invalid preset identifier");try{return respond(http::status::ok,v,delete_parameter_preset(app,user->id,preset_id));}catch(const std::exception& e){return problem(v,http::status::not_found,e.what());}}
  if(p.starts_with("/api/v1/refinements/")){
   std::string rem=p.substr(std::string("/api/v1/refinements/").size());const auto slash=rem.find('/');const auto session_id=rem.substr(0,slash);if(!valid_id(session_id))return problem(v,http::status::bad_request,"Invalid refinement identifier");const auto action=slash==std::string::npos?std::string():rem.substr(slash+1);
   try{
    if(action.empty()&&req.method()==http::verb::get)return respond(http::status::ok,v,refinement_json(app,session_id,user->id));
    if(action=="interventions"&&req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,apply_refinement(app,session_id,user->id,req.body()));}
    if(action=="preview"&&req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::ok,v,preview_refinement(app,session_id,user->id,req.body()),"image/png");}
    if(action=="confirm"&&req.method()==http::verb::post)return respond(http::status::ok,v,confirm_refinement(app,session_id,user->id));
    if(action=="metrics/runs"&&req.method()==http::verb::get)return respond(http::status::ok,v,refinement_metric_runs_json(app,session_id,user->id));
    if(action=="metrics"){
     if(req.method()==http::verb::get)return respond(http::status::ok,v,refinement_latest_metrics(app,session_id,user->id));
     if(req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");Db context(app.root/"morfo.sqlite3");Stmt row(context.p,"SELECT image_id,base_profile_run_id FROM morfo_refinement_sessions WHERE id=? AND user_id=? AND status='CONFIRMED'");row.bind(1,session_id);row.bind(2,user->id);if(sqlite3_step(row.p)!=SQLITE_ROW)throw Failure("Confirmed refinement not found");return respond(http::status::created,v,morfo_start_metrics(app,col(row.p,0),col(row.p,1),user->id,req.body(),session_id));}
    }
    if(action=="undo"&&req.method()==http::verb::post)return respond(http::status::ok,v,navigate_refinement(app,session_id,user->id,false));
    if(action=="redo"&&req.method()==http::verb::post)return respond(http::status::ok,v,navigate_refinement(app,session_id,user->id,true));
    if(action=="metadata"&&req.method()==http::verb::get){auto response=respond(http::status::ok,v,refinement_json(app,session_id,user->id));response.set(http::field::content_disposition,"attachment; filename=\"morfo-refinement-"+session_id+".json\"");return response;}
    if(action=="export.zip"&&req.method()==http::verb::get){auto response=respond(http::status::ok,v,refinement_bundle(app,session_id,user->id),"application/zip");response.set(http::field::content_disposition,"attachment; filename=\"morfo-refinement-"+session_id+".zip\"");return response;}
    if((action=="mask"||action=="overlay"||action=="before"||action=="difference")&&req.method()==http::verb::get){std::string mime;return respond(http::status::ok,v,refinement_artifact(app,session_id,user->id,action,mime),mime);}
   }catch(const std::exception& e){const std::string message=e.what();return problem(v,message=="Refinement session not found"?http::status::not_found:http::status::unprocessable_entity,message);}
  }
  if(p.starts_with("/api/v1/root-metrics/")){
   const std::string metric_id=p.substr(std::string("/api/v1/root-metrics/").size());if(!valid_id(metric_id))return problem(v,http::status::bad_request,"Invalid metric identifier");
   try{if(req.method()==http::verb::get)return respond(http::status::ok,v,morfo_metric_run_json(app,metric_id,user->id));if(req.method()==http::verb::patch){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::ok,v,update_morfo_metric_run(app,metric_id,user->id,req.body()));}if(req.method()==http::verb::delete_)return respond(http::status::ok,v,delete_morfo_metric_run(app,metric_id,user->id));}
   catch(const std::exception& e){return problem(v,std::string(e.what())=="Root metric run not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
  }
  if(p.starts_with("/api/v1/calibrations/")){
   const std::string calibration_id=p.substr(std::string("/api/v1/calibrations/").size());if(!valid_id(calibration_id))return problem(v,http::status::bad_request,"Invalid calibration identifier");
   try{if(req.method()==http::verb::patch){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::ok,v,update_image_calibration(app,calibration_id,user->id,req.body()));}if(req.method()==http::verb::delete_)return respond(http::status::ok,v,delete_image_calibration(app,calibration_id,user->id));}
   catch(const std::exception& e){return problem(v,std::string(e.what())=="Calibration not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
  }
  if(p.starts_with("/api/v1/analysis-runs/")){
   std::string rem=p.substr(std::string("/api/v1/analysis-runs/").size());
   auto slash=rem.find('/');
   auto run_id=rem.substr(0,slash);
   if(!valid_id(run_id))return problem(v,http::status::bad_request,"Invalid run identifier");
   try{
    if(slash==std::string::npos){
     if(req.method()==http::verb::patch){
      if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");
      return respond(http::status::ok,v,update_analysis_run(app,run_id,user->id,req.body()));
     }
     if(req.method()==http::verb::delete_){
      return respond(http::status::ok,v,delete_analysis_run(app,run_id,user->id));
     }
    }else if(rem.substr(slash+1)=="confirm-all"){
     if(req.method()==http::verb::post){
      return respond(http::status::ok,v,confirm_all_reviews(app,run_id,user->id,req.body()));
     }
    }
   }catch(const Failure& e){
    std::string msg=e.what();
    if(msg=="Analysis run not found")return problem(v,http::status::not_found,msg);
    return problem(v,http::status::unprocessable_entity,msg);
   }
  }
  if(p.starts_with("/api/v1/analysis-regions/")){
   std::string rem=p.substr(std::string("/api/v1/analysis-regions/").size());auto slash=rem.find('/');auto region_id=rem.substr(0,slash);
   if(!valid_id(region_id)||slash==std::string::npos||rem.substr(slash+1)!="reviews")return problem(v,http::status::bad_request,"Invalid review resource");
   try{
    if(req.method()==http::verb::get)return respond(http::status::ok,v,review_history(app,region_id,user->id));
    if(req.method()==http::verb::post){
     if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");
     return respond(http::status::created,v,add_review(app,region_id,user->id,req.body()));
    }
   }catch(const Failure& e){std::string message=e.what();if(message=="Review conflict")return problem(v,http::status::conflict,message);if(message=="Region not found")return problem(v,http::status::not_found,message);return problem(v,http::status::unprocessable_entity,message);}
  }
  if(p.starts_with("/api/v1/images/")){
   std::string rem=p.substr(std::string("/api/v1/images/").size());auto slash=rem.find('/');auto id=rem.substr(0,slash);if(!valid_id(id))return problem(v,http::status::bad_request,"Invalid image identifier");
   if(slash==std::string::npos){
    try{
     if(req.method()==http::verb::get)return respond(http::status::ok,v,one_image(app,id,user->id));
     if(req.method()==http::verb::patch){
      if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");
      return respond(http::status::ok,v,update_image_metadata(app,id,user->id,req.body()));
     }
     if(req.method()==http::verb::delete_){
      return respond(http::status::ok,v,delete_image(app,id,user->id));
     }
    }catch(const Failure& e){
     std::string msg=e.what();
     if(msg=="Image not found")return problem(v,http::status::not_found,msg);
     return problem(v,http::status::unprocessable_entity,msg);
    }
   }else{
    auto kind=rem.substr(slash+1);
    if(kind=="calibrations"){
     try{if(req.method()==http::verb::get)return respond(http::status::ok,v,image_calibrations_json(app,id,user->id));if(req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,create_image_calibration(app,id,user->id,req.body()));}}
     catch(const std::exception& e){return problem(v,std::string(e.what())=="Image not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
    }
    if(kind=="profiles/roots-exp003"){
     try{
      if(req.method()==http::verb::post)return respond(http::status::created,v,morfo_start_profile(app,id,user->id));
      if(req.method()==http::verb::get)return respond(http::status::ok,v,morfo_profile_list(app,id,user->id));
     }catch(const std::exception& e){return problem(v,std::string(e.what())=="Image not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
    }
    if(kind=="refinements"){
     try{if(req.method()==http::verb::get)return respond(http::status::ok,v,image_refinements(app,id,user->id));if(req.method()==http::verb::post){if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");return respond(http::status::created,v,create_refinement(app,id,user->id,req.body()));}}catch(const std::exception& e){const std::string message=e.what();return problem(v,message=="Profile run not found"||message=="Image not found"?http::status::not_found:http::status::unprocessable_entity,message);}
    }
    if(kind.starts_with("profiles/roots-exp003/")&&kind.ends_with("/metrics/runs")){
     const auto inner=kind.substr(std::string("profiles/roots-exp003/").size());const auto profile_run_id=inner.substr(0,inner.size()-std::string("/metrics/runs").size());if(!valid_id(profile_run_id))return problem(v,http::status::bad_request,"Invalid profile run identifier");
     if(req.method()==http::verb::get)return respond(http::status::ok,v,morfo_metric_runs_json(app,id,profile_run_id,user->id));
    }
    if(kind.starts_with("profiles/roots-exp003/")&&req.method()==http::verb::delete_){const auto profile_run_id=kind.substr(std::string("profiles/roots-exp003/").size());if(!valid_id(profile_run_id))return problem(v,http::status::bad_request,"Invalid profile run identifier");try{return respond(http::status::ok,v,morfo_delete_profile(app,id,profile_run_id,user->id));}catch(const std::exception& e){return problem(v,std::string(e.what())=="Profile run not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}}
    if(kind.starts_with("profiles/roots-exp003/")&&(kind.ends_with("/metrics/export.csv")||kind.ends_with("/metrics/export.json"))){
     const bool csv=kind.ends_with("/export.csv");const std::string ending=csv?"/metrics/export.csv":"/metrics/export.json";
     const auto inner=kind.substr(std::string("profiles/roots-exp003/").size());const auto profile_run_id=inner.substr(0,inner.size()-ending.size());
     if(!valid_id(profile_run_id))return problem(v,http::status::bad_request,"Invalid profile run identifier");
     try{if(req.method()==http::verb::get){auto r=respond(http::status::ok,v,csv?morfo_metrics_csv(app,id,profile_run_id,user->id):morfo_latest_metrics(app,id,profile_run_id,user->id),csv?"text/csv; charset=utf-8":"application/json; charset=utf-8");r.set(http::field::content_disposition,"attachment; filename=\"morfo-morfometria-"+profile_run_id+(csv?".csv\"":".json\""));return r;}}
     catch(const std::exception& e){return problem(v,http::status::not_found,e.what());}
    }
    if(kind.starts_with("profiles/roots-exp003/")&&kind.ends_with("/metrics")){
     const auto inner=kind.substr(std::string("profiles/roots-exp003/").size());
     const auto profile_run_id=inner.substr(0,inner.size()-std::string("/metrics").size());
     if(!valid_id(profile_run_id))return problem(v,http::status::bad_request,"Invalid profile run identifier");
     try{
      if(req.method()==http::verb::post){
       if(req[http::field::content_type].find("application/json")!=0)return problem(v,http::status::unsupported_media_type,"Use application/json");
       return respond(http::status::created,v,morfo_start_metrics(app,id,profile_run_id,user->id,req.body()));
      }
      if(req.method()==http::verb::get)return respond(http::status::ok,v,morfo_latest_metrics(app,id,profile_run_id,user->id));
     }catch(const std::exception& e){return problem(v,std::string(e.what())=="Profile run not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
    }
    if(kind.starts_with("profiles/roots-exp003/")&&req.method()==http::verb::get){
     try{std::string mime;auto bytes=morfo_profile_artifact(app,id,user->id,kind.substr(std::string("profiles/roots-exp003/").size()),mime);return respond(http::status::ok,v,std::move(bytes),mime);}
     catch(const std::exception& e){return problem(v,http::status::not_found,e.what());}
    }
    if(req.method()==http::verb::post&&kind=="analysis"){
     std::string algo=args.contains("algorithm")?args["algorithm"]:(req.body().empty()?"":json_extract_string(req.body(),"algorithm"));
     try{return respond(http::status::created,v,start_analysis(app,id,user->id,algo));}catch(const Failure& e){return problem(v,std::string(e.what())=="Image not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
    }
    if(req.method()==http::verb::get){
     try{
      if(kind=="analysis")return respond(http::status::ok,v,analysis_json(app,id,user->id,args["run_id"]));
      if(kind=="analysis-runs")return respond(http::status::ok,v,analysis_runs_json(app,id,user->id));
      if(kind=="analysis-export"||kind=="analysis-export.csv"){
       auto r=respond(http::status::ok,v,analysis_export_csv(app,id,user->id,args["run_id"]),"text/csv; charset=utf-8");
       r.set(http::field::content_disposition,"attachment; filename=\"rizoma-analise-"+id+".csv\"");
       return r;
      }
      if(kind=="pyramid")return respond(http::status::ok,v,pyramid_manifest(app,id,user->id));
      if(kind.starts_with("tiles/")){std::string mime;auto tile=pyramid_tile(app,id,kind,mime,user->id);return respond(http::status::ok,v,std::move(tile),mime);}
      if(kind=="original"||kind=="original-view"||kind=="preview"||kind=="grayscale"){
       std::string mime;std::string binary=media(app,id,kind,mime,user->id);return respond(http::status::ok,v,std::move(binary),mime);
      }
     }catch(const Failure&){return problem(v,http::status::not_found,"Image not found");}
    }
   }
  }
 }

 if(req.method()==http::verb::get){const std::map<std::string,std::pair<std::string,std::string>> assets={{"/",{"index.html","text/html; charset=utf-8"}},{"/index.html",{"index.html","text/html; charset=utf-8"}},{"/style.css",{"style.css","text/css; charset=utf-8"}},{"/app.js",{"app.js","application/javascript; charset=utf-8"}},{"/morfo-profiles.js",{"morfo-profiles.js","application/javascript; charset=utf-8"}}};auto it=assets.find(p);if(it!=assets.end())try{return respond(http::status::ok,v,slurp(app.web/it->second.first),it->second.second);}catch(...){return problem(v,http::status::internal_server_error,"Frontend unavailable");}}
 return problem(v,http::status::not_found,"Resource not found");
}
static http::request<http::string_body> request_header(const http::request<http::buffer_body>& source){
 http::request<http::string_body> target{source.method(),source.target(),source.version()};
 for(const auto& field:source)target.set(field.name_string(),field.value());
 return target;
}
static bool allowed_origin(const App& app,const http::request<http::string_body>& request){
 const std::string origin=std::string(request[http::field::origin]),host=std::string(request[http::field::host]);
 return origin.empty()||origin=="http://"+host||origin=="http://localhost:"+std::to_string(app.port)||origin=="http://127.0.0.1:"+std::to_string(app.port)||(!app.host.empty()&&origin=="http://"+app.host+":"+std::to_string(app.port));
}
static http::status ingest_status(std::string_view message){
 if(message.find("already registered")!=std::string_view::npos)return http::status::conflict;
 if(message.find("Formato")!=std::string_view::npos||message.find("Only PNG")!=std::string_view::npos)return http::status::unsupported_media_type;
 if(message.find("8 GiB")!=std::string_view::npos)return http::status::payload_too_large;
 return http::status::unprocessable_entity;
}
static void session(tcp::socket socket,App& app){
 struct ActiveGuard {App& app;~ActiveGuard(){--app.active;}} active_guard{app};
 try{
 boost::system::error_code ep_ec;
 auto ep = socket.remote_endpoint(ep_ec);
 std::string remote_client_ip = ep_ec ? "127.0.0.1" : ep.address().to_string();
 beast::tcp_stream stream(std::move(socket));stream.expires_after(std::chrono::minutes(5));beast::flat_buffer buffer;http::request_parser<http::buffer_body> parser;parser.header_limit(16*1024);parser.body_limit(MAX_UPLOAD);beast::error_code ec;http::read_header(stream,buffer,parser,ec);
 if(ec)return;
 auto req=request_header(parser.get());req.set("X-Client-IP", remote_client_ip);Response r=problem(req.version(),http::status::internal_server_error,"Server error");
 const std::string target(req.target());const std::string path=target.substr(0,target.find('?'));
 const bool upload=req.method()==http::verb::post&&path=="/api/v1/images";
 if(!allowed_origin(app,req))r=problem(req.version(),http::status::forbidden,"Cross-origin requests are disabled");
 else if(upload){
  const auto user=authenticate(app,req);
  if(!user)r=problem(req.version(),http::status::unauthorized,"Autenticação necessária. Solicite uma chave de acesso para seu e-mail.");
  else if((req.find("X-Morfo-Client")==req.end()||req["X-Morfo-Client"]!="web-v0.2.0")&&
          (req.find("X-Rizoma-Client")==req.end()||req["X-Rizoma-Client"]!="web-v0.2.0"))r=problem(req.version(),http::status::forbidden,"Client header required");
  else if(req[http::field::content_type]!="application/octet-stream")r=problem(req.version(),http::status::unsupported_media_type,"Use application/octet-stream");
  else{
   const fs::path staged=app.root/"staging"/("upload-"+rand_id()+".tmp");
   struct StageGuard {fs::path path;~StageGuard(){std::error_code ignored;fs::remove(path,ignored);}} guard{staged};
   try{
    fs::create_directories(staged.parent_path());std::ofstream output(staged,std::ios::binary|std::ios::trunc);if(!output)throw Failure("Could not create upload staging file");
    std::array<char,64*1024> chunk{};std::uint64_t total=0;
    while(!parser.is_done()){
     parser.get().body().data=chunk.data();parser.get().body().size=chunk.size();http::read_some(stream,buffer,parser,ec);
     const auto count=chunk.size()-parser.get().body().size;
     if(count){total+=count;if(total>MAX_UPLOAD)throw Failure("File exceeds 8 GiB upload limit");output.write(chunk.data(),static_cast<std::streamsize>(count));if(!output)throw Failure("Disk write failed during upload");}
     if(ec==http::error::need_buffer)ec={};
     if(ec)throw Failure("Interrupted image upload");
    }
    output.close();if(!output)throw Failure("Could not finish staged upload");::chmod(staged.c_str(),0600);
    auto arguments=params(target);const auto ingested=ingest_file(app,staged,arguments["filename"],arguments["sample_id"],arguments["capture_date"],user->id,user->email);
    r=respond(http::status::created,req.version(),ingested.json);
   }catch(const Failure& error){r=problem(req.version(),ingest_status(error.what()),error.what());}
   catch(const std::exception& error){std::cerr<<"Progressive ingest error: "<<error.what()<<"\n";r=problem(req.version(),http::status::unprocessable_entity,error.what());}
  }
 }else{
  try{
   std::string body;std::array<char,16*1024> chunk{};
   while(!parser.is_done()){
    parser.get().body().data=chunk.data();parser.get().body().size=chunk.size();http::read_some(stream,buffer,parser,ec);
    const auto count=chunk.size()-parser.get().body().size;if(body.size()+count>MAX_CONTROL_BODY)throw Failure("Request body exceeds 1 MiB");body.append(chunk.data(),count);
    if(ec==http::error::need_buffer)ec={};
    if(ec)throw Failure("Interrupted request body");
   }
   req.body()=std::move(body);r=handle(app,req);
  }catch(const Failure& error){r=problem(req.version(),http::status::payload_too_large,error.what());}
  catch(const std::exception& e){std::cerr<<"Request error: "<<e.what()<<"\n";r=problem(req.version(),http::status::internal_server_error,"Unexpected server error");}
 }
 http::write(stream,r,ec);stream.socket().shutdown(tcp::socket::shutdown_both,ec);
 }catch(const std::exception& e){std::cerr<<"Connection error: "<<e.what()<<"\n";}
}
int main(int argc,char** argv){try{
 ::umask(0077);App app;app.root="./var";app.web="./web";bool migrate_only=false;
 std::string add_user_email;
 std::string add_user_name;
 std::string add_user_role="admin";
 bool list_users=false;
 std::string deactivate_email;
 std::string activate_email;

 TIFFSetWarningHandler(nullptr);TIFFSetErrorHandler(nullptr);
 for(int i=1;i<argc;i++){
  std::string a=argv[i];
  if(a=="--data-dir"&&i+1<argc)app.root=argv[++i];
  else if(a=="--web-dir"&&i+1<argc)app.web=argv[++i];
  else if(a=="--host"&&i+1<argc)app.host=argv[++i];
  else if(a=="--port"&&i+1<argc){
   int port=std::stoi(argv[++i]);
   if(port<1024||port>65535)throw Failure("Port out of range");
   app.port=static_cast<unsigned short>(port);
  }
  else if(a=="--migrate-only")migrate_only=true;
  else if(a=="--add-user"&&i+1<argc)add_user_email=argv[++i];
  else if(a=="--name"&&i+1<argc)add_user_name=argv[++i];
  else if(a=="--role"&&i+1<argc)add_user_role=argv[++i];
  else if(a=="--list-users")list_users=true;
  else if(a=="--deactivate-user"&&i+1<argc)deactivate_email=argv[++i];
  else if(a=="--activate-user"&&i+1<argc)activate_email=argv[++i];
  else throw Failure("Usage: morfo-server [--host IP] [--data-dir DIRECTORY] [--web-dir DIRECTORY] [--port 1024..65535] [--migrate-only] [--add-user EMAIL [--name NAME] [--role ROLE]] [--list-users] [--deactivate-user EMAIL] [--activate-user EMAIL]");
 }
 initialize(app);
 if(migrate_only){std::cout<<"MORFO SQLite migrations complete\n";return 0;}
 if(!add_user_email.empty()){
  Db db(app.root/"morfo.sqlite3");
  if(add_user_name.empty())add_user_name=name_from_email(add_user_email);
  if(app.auth_service.add_user(db.p,add_user_email,add_user_name,add_user_role)){
   std::cout<<"Usuário registrado com sucesso: "<<add_user_email<<" (Nome: "<<add_user_name<<", Perfil: "<<add_user_role<<")\n";
   return 0;
  }else{
   std::cerr<<"Falha ao registrar usuário.\n";
   return 1;
  }
 }
 if(list_users){
  Db db(app.root/"morfo.sqlite3");
  auto users=app.auth_service.list_users(db.p);
  std::cout<<"Usuários cadastrados ("<<users.size()<<"):\n";
  for(const auto& u:users){
   std::cout<<" - "<<u.email<<" ["<<u.role<<"] "
            <<(u.is_active?"ATIVO":"INATIVO")
            <<" - "<<u.name
            <<" (Criado em: "<<u.created_at
            <<", Último login: "<<(u.last_login_at.empty()?"nunca":u.last_login_at)<<")\n";
  }
  return 0;
 }
 if(!deactivate_email.empty()){
  Db db(app.root/"morfo.sqlite3");
  if(app.auth_service.set_user_active(db.p,deactivate_email,false)){
   std::cout<<"Usuário desativado: "<<deactivate_email<<"\n";
   return 0;
  }else{
   std::cerr<<"Falha ao desativar usuário.\n";
   return 1;
  }
 }
 if(!activate_email.empty()){
  Db db(app.root/"morfo.sqlite3");
  if(app.auth_service.set_user_active(db.p,activate_email,true)){
   std::cout<<"Usuário ativado: "<<activate_email<<"\n";
   return 0;
  }else{
   std::cerr<<"Falha ao ativar usuário.\n";
   return 1;
  }
 }
 asio::io_context io;
 boost::system::error_code ec;auto addr=asio::ip::make_address(app.host,ec);
 if(ec)throw Failure("Invalid IP address for --host: "+app.host);
 tcp::acceptor acceptor(io,{addr,app.port});
 std::cout<<"MORFO v0.1.0 — http://"<<app.host<<":"<<app.port<<(app.host=="127.0.0.1"?" (loopback only)":app.host=="0.0.0.0"?" (all interfaces / LAN)":" (local network)")<<"\n"<<"Data dir: "<<fs::absolute(app.root)<<"\n"<<std::flush;
 for(;;){tcp::socket s(io);acceptor.accept(s);if(app.active.load()>=4){s.close();continue;}++app.active;std::thread(session,std::move(s),std::ref(app)).detach();}
 }catch(const std::exception& e){std::cerr<<"MORFO fatal: "<<e.what()<<"\n";return 1;}}
