#include "rizoma.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <openssl/evp.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <sstream>
#include <stdexcept>
#include <tiffio.h>
namespace rizoma {
namespace {
struct JpegError {jpeg_error_mgr pub; jmp_buf jump;};
extern "C" void jpeg_failed(j_common_ptr state){longjmp(reinterpret_cast<JpegError*>(state->err)->jump,1);}
Image tiff_read(const fs::path& p){
 TIFF* handle=TIFFOpen(p.c_str(),"r");if(!handle)throw std::runtime_error("TIFF inválido ou inacessível");
 uint32_t w=0,h=0; TIFFGetField(handle,TIFFTAG_IMAGEWIDTH,&w);TIFFGetField(handle,TIFFTAG_IMAGELENGTH,&h);
 if(w==0||h==0||static_cast<std::uint64_t>(w)*h>48'000'000){TIFFClose(handle);throw std::runtime_error("Dimensões TIFF inválidas");}
 std::vector<std::uint32_t> buffer(static_cast<std::size_t>(w)*h);
 if(!TIFFReadRGBAImageOriented(handle,w,h,buffer.data(),ORIENTATION_TOPLEFT,0)){TIFFClose(handle);throw std::runtime_error("Não foi possível decodificar TIFF");}
 TIFFClose(handle);Image img{static_cast<int>(w),static_cast<int>(h),{}};img.pixels.resize(buffer.size());
 for(std::size_t i=0;i<buffer.size();++i)img.pixels[i]={TIFFGetR(buffer[i]),TIFFGetG(buffer[i]),TIFFGetB(buffer[i])};
 return img;
}
Image png_read(const fs::path& p){
 png_image pi{};pi.version=PNG_IMAGE_VERSION;
 if(!png_image_begin_read_from_file(&pi,p.c_str()))throw std::runtime_error("PNG inválido");
 if(pi.width==0||pi.height==0||std::uint64_t(pi.width)*pi.height>48'000'000){png_image_free(&pi);throw std::runtime_error("Dimensões PNG inválidas");}
 Image img{static_cast<int>(pi.width),static_cast<int>(pi.height),{}};pi.format=PNG_FORMAT_RGB;
 std::vector<std::uint8_t> buf(PNG_IMAGE_SIZE(pi));
 if(!png_image_finish_read(&pi,nullptr,buf.data(),0,nullptr)){png_image_free(&pi);throw std::runtime_error("Erro de leitura PNG");}
 png_image_free(&pi);img.pixels.resize(std::size_t(img.width)*img.height);
 for(std::size_t i=0;i<img.pixels.size();i++)img.pixels[i]={buf[3*i],buf[3*i+1],buf[3*i+2]};
 return img;
}
Image jpeg_read(const fs::path& p){
 std::FILE* f=std::fopen(p.c_str(),"rb");if(!f)throw std::runtime_error("Erro ao abrir JPEG");
 jpeg_decompress_struct ci{};JpegError er{};ci.err=jpeg_std_error(&er.pub);er.pub.error_exit=jpeg_failed;
 if(setjmp(er.jump)){jpeg_destroy_decompress(&ci);std::fclose(f);throw std::runtime_error("Erro na decodificação JPEG");}
 jpeg_create_decompress(&ci);jpeg_stdio_src(&ci,f);jpeg_read_header(&ci,TRUE);
 if(ci.image_width==0||ci.image_height==0||std::uint64_t(ci.image_width)*ci.image_height>48'000'000){jpeg_destroy_decompress(&ci);std::fclose(f);throw std::runtime_error("Dimensões JPEG inválidas");}
 ci.out_color_space=JCS_RGB;jpeg_start_decompress(&ci);Image img{static_cast<int>(ci.output_width),static_cast<int>(ci.output_height),{}};
 img.pixels.resize(std::size_t(img.width)*img.height);std::vector<std::uint8_t> row(std::size_t(img.width)*3);
 while(ci.output_scanline<ci.output_height){JSAMPROW line=row.data();jpeg_read_scanlines(&ci,&line,1);auto y=ci.output_scanline-1;
 for(int x=0;x<img.width;x++){auto idx=std::size_t(y)*img.width+x;img.pixels[idx]={row[3*x],row[3*x+1],row[3*x+2]};}}
 jpeg_finish_decompress(&ci);jpeg_destroy_decompress(&ci);std::fclose(f);return img;
}
void write_file(const fs::path& p,int w,int h,int fmt,const std::uint8_t* bytes){
 if(w<=0||h<=0||!bytes)throw std::runtime_error("Tentativa de escrever imagem vazia");
 fs::create_directories(p.parent_path());png_image im{};im.version=PNG_IMAGE_VERSION;im.width=w;im.height=h;im.format=fmt;
 if(!png_image_write_to_file(&im,p.c_str(),0,bytes,0,nullptr))throw std::runtime_error("Erro ao salvar PNG: "+p.string());
}
}
Image read_rgb(const fs::path& p){
 std::ifstream stream(p,std::ios::binary);if(!stream)throw std::runtime_error("Arquivo de origem ausente");
 std::array<unsigned char,8> head{};stream.read(reinterpret_cast<char*>(head.data()),head.size());
 if(stream.gcount()<4)throw std::runtime_error("Arquivo muito curto");
 if(head[0]==0x89&&head[1]=='P'&&head[2]=='N'&&head[3]=='G')return png_read(p);
 if(head[0]==0xff&&head[1]==0xd8)return jpeg_read(p);
 if((head[0]=='I'&&head[1]=='I'&&head[2]==42&&head[3]==0)||(head[0]=='M'&&head[1]=='M'&&head[2]==0&&head[3]==42))return tiff_read(p);
 throw std::runtime_error("Formato aceito: PNG, JPEG ou TIFF de 8 bits RGB (conversão de visualização)");
}
void write_png(const fs::path& p,const Image& img){
 std::vector<std::uint8_t> buf(img.pixels.size()*3);
 for(std::size_t i=0;i<img.pixels.size();++i){buf[3*i]=img.pixels[i].r;buf[3*i+1]=img.pixels[i].g;buf[3*i+2]=img.pixels[i].b;}
 write_file(p,img.width,img.height,PNG_FORMAT_RGB,buf.data());
}
void write_png(const fs::path& p,const Gray& img){write_file(p,img.width,img.height,PNG_FORMAT_GRAY,img.pixels.data());}
std::string sha256_file(const fs::path& p){
 std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("Erro ao abrir arquivo para hash");
 EVP_MD_CTX* ctx=EVP_MD_CTX_new();if(!ctx)throw std::runtime_error("EVP_MD_CTX_new falhou");
 if(EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)!=1){EVP_MD_CTX_free(ctx);throw std::runtime_error("SHA256 falhou");}
 std::array<char,65536> chunk{};while(in){in.read(chunk.data(),chunk.size());auto n=in.gcount();if(n>0)EVP_DigestUpdate(ctx,chunk.data(),static_cast<std::size_t>(n));}
 std::array<unsigned char,EVP_MAX_MD_SIZE> buf{};unsigned int n=0;EVP_DigestFinal_ex(ctx,buf.data(),&n);EVP_MD_CTX_free(ctx);
 std::ostringstream out;for(unsigned int i=0;i<n;i++)out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned int>(buf[i]);return out.str();
}
}
