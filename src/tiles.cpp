#include "tiles.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <openssl/evp.h>
#include <png.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <tiffio.h>

namespace rizoma::tiles {
namespace fs=std::filesystem;
namespace {

using RowCallback=std::function<void(std::uint64_t,const std::uint8_t*)>;
struct Dimensions {std::uint64_t width{},height{};std::string format,transform;};

std::uint64_t divceil(std::uint64_t value,std::uint64_t divisor){return value/divisor+(value%divisor!=0);}
std::uint64_t checked_multiply(std::uint64_t left,std::uint64_t right){
 if(right&&left>std::numeric_limits<std::uint64_t>::max()/right)throw std::runtime_error("Overflow de dimensoes");
 return left*right;
}
std::size_t checked_size(std::uint64_t value){
 if(value>std::numeric_limits<std::size_t>::max())throw std::runtime_error("Dimensao superior ao endereco disponivel");
 return static_cast<std::size_t>(value);
}
std::string sha256_file(const fs::path& path){
 std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Erro ao abrir arquivo para hash");
 std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
 if(!context||EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("SHA-256 indisponivel");
 std::array<char,65536> block{};
 while(input){input.read(block.data(),block.size());const auto count=input.gcount();if(count>0&&EVP_DigestUpdate(context.get(),block.data(),static_cast<std::size_t>(count))!=1)throw std::runtime_error("Falha SHA-256");}
 std::array<unsigned char,EVP_MAX_MD_SIZE> digest{};unsigned int length=0;
 if(EVP_DigestFinal_ex(context.get(),digest.data(),&length)!=1)throw std::runtime_error("Falha SHA-256");
 std::ostringstream out;for(unsigned int i=0;i<length;++i)out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned int>(digest[i]);return out.str();
}
std::string tile_name(std::uint64_t x,std::uint64_t y){return std::to_string(x)+"_"+std::to_string(y)+".png";}

void store_png(const fs::path& path,const std::uint8_t* data,std::uint32_t width,std::uint32_t height){
 if(!width||!height)throw std::runtime_error("Tile vazio");
 png_image image{};image.version=PNG_IMAGE_VERSION;image.width=width;image.height=height;image.format=PNG_FORMAT_RGB;
 if(!png_image_write_to_file(&image,path.c_str(),0,data,0,nullptr))throw std::runtime_error("Falha ao gravar tile PNG");
}
struct SmallImage {std::uint32_t width{},height{};std::vector<std::uint8_t> rgb;};
SmallImage load_tile(const fs::path& path,std::uint32_t maximum){
 png_image image{};image.version=PNG_IMAGE_VERSION;
 if(!png_image_begin_read_from_file(&image,path.c_str()))throw std::runtime_error("Tile ilegivel");
 if(!image.width||!image.height||image.width>maximum||image.height>maximum){png_image_free(&image);throw std::runtime_error("Tile invalido");}
 image.format=PNG_FORMAT_RGB;SmallImage result{image.width,image.height,{}};
 result.rgb.resize(checked_size(checked_multiply(checked_multiply(image.width,image.height),3)));
 if(!png_image_finish_read(&image,nullptr,result.rgb.data(),0,nullptr)){png_image_free(&image);throw std::runtime_error("Tile danificado");}
 png_image_free(&image);return result;
}

struct JpegError {jpeg_error_mgr pub;jmp_buf jump;};
extern "C" void tiles_jpeg_failed(j_common_ptr state){longjmp(reinterpret_cast<JpegError*>(state->err)->jump,1);}

Dimensions decode_png(const fs::path& path,const RowCallback& row){
 FILE* file=std::fopen(path.c_str(),"rb");if(!file)throw std::runtime_error("PNG inacessivel");
 png_structp png=png_create_read_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
 if(!png){std::fclose(file);throw std::runtime_error("Falha no leitor PNG");}
 png_infop info=png_create_info_struct(png);
 if(!info){png_destroy_read_struct(&png,nullptr,nullptr);std::fclose(file);throw std::runtime_error("Falha no leitor PNG");}
 if(setjmp(png_jmpbuf(png))){png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);throw std::runtime_error("Erro ao decodificar PNG");}
 png_init_io(png,file);png_read_info(png,info);
 const auto width=png_get_image_width(png,info),height=png_get_image_height(png,info);
 const int depth=png_get_bit_depth(png,info),color=png_get_color_type(png,info);
 if(!width||!height)png_error(png,"Dimensoes invalidas");
 if(depth==16)png_set_strip_16(png);
 if(color==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);
 if(color==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(png);
 if(png_get_valid(png,info,PNG_INFO_tRNS))png_set_tRNS_to_alpha(png);
 if(color==PNG_COLOR_TYPE_GRAY||color==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(png);
 if((color&PNG_COLOR_MASK_ALPHA)||png_get_valid(png,info,PNG_INFO_tRNS))png_set_strip_alpha(png);
 png_read_update_info(png,info);
 if(png_get_channels(png,info)!=3||png_get_rowbytes(png,info)!=static_cast<png_size_t>(width)*3)png_error(png,"Formato PNG nao suportado");
 std::vector<std::uint8_t> buffer(checked_size(checked_multiply(width,3)));
 for(std::uint64_t y=0;y<height;++y){png_read_row(png,buffer.data(),nullptr);row(y,buffer.data());}
 png_read_end(png,nullptr);png_destroy_read_struct(&png,&info,nullptr);std::fclose(file);
 return {width,height,"PNG",depth==16?"PREVIEW_16_TO_8_HIGH_BYTE":"RGB_8_VIEW"};
}

Dimensions decode_jpeg(const fs::path& path,const RowCallback& row){
 FILE* file=std::fopen(path.c_str(),"rb");if(!file)throw std::runtime_error("JPEG inacessivel");
 jpeg_decompress_struct state{};JpegError error{};state.err=jpeg_std_error(&error.pub);error.pub.error_exit=tiles_jpeg_failed;
 if(setjmp(error.jump)){jpeg_destroy_decompress(&state);std::fclose(file);throw std::runtime_error("JPEG danificado");}
 jpeg_create_decompress(&state);jpeg_stdio_src(&state,file);jpeg_read_header(&state,TRUE);state.out_color_space=JCS_RGB;jpeg_start_decompress(&state);
 const std::uint64_t width=state.output_width,height=state.output_height;
 if(!width||!height||state.output_components!=3){jpeg_destroy_decompress(&state);std::fclose(file);throw std::runtime_error("JPEG invalido");}
 std::vector<std::uint8_t> buffer(checked_size(checked_multiply(width,3)));
 while(state.output_scanline<state.output_height){const auto y=state.output_scanline;JSAMPROW pointer=buffer.data();jpeg_read_scanlines(&state,&pointer,1);row(y,buffer.data());}
 jpeg_finish_decompress(&state);jpeg_destroy_decompress(&state);std::fclose(file);return {width,height,"JPEG","RGB_8_VIEW"};
}

Dimensions decode_tiff(const fs::path& path,const RowCallback& row){
 TIFF* tiff=TIFFOpen(path.c_str(),"r");if(!tiff)throw std::runtime_error("TIFF inacessivel");
 uint32_t width=0,height=0;uint16_t bits=0,samples=0,photo=0,planar=0,orientation=ORIENTATION_TOPLEFT;
 TIFFGetField(tiff,TIFFTAG_IMAGEWIDTH,&width);TIFFGetField(tiff,TIFFTAG_IMAGELENGTH,&height);
 TIFFGetFieldDefaulted(tiff,TIFFTAG_BITSPERSAMPLE,&bits);TIFFGetFieldDefaulted(tiff,TIFFTAG_SAMPLESPERPIXEL,&samples);
 TIFFGetFieldDefaulted(tiff,TIFFTAG_PHOTOMETRIC,&photo);TIFFGetFieldDefaulted(tiff,TIFFTAG_PLANARCONFIG,&planar);TIFFGetFieldDefaulted(tiff,TIFFTAG_ORIENTATION,&orientation);
 const bool supported_photo=photo==PHOTOMETRIC_RGB||photo==PHOTOMETRIC_MINISBLACK||photo==PHOTOMETRIC_MINISWHITE;
 if(!width||!height||(bits!=8&&bits!=16)||(samples!=1&&samples!=3&&samples!=4)||planar!=PLANARCONFIG_CONTIG||orientation!=ORIENTATION_TOPLEFT||!supported_photo||
    (photo==PHOTOMETRIC_RGB&&samples<3)||(photo!=PHOTOMETRIC_RGB&&samples!=1)){
  TIFFClose(tiff);throw std::runtime_error("TIFF requer RGB/cinza 8/16 bits, contiguo e orientacao TOPLEFT");
 }
 std::vector<std::uint8_t> rgb(checked_size(checked_multiply(width,3)));
 auto convert=[&](const std::uint8_t* source){
  for(uint32_t x=0;x<width;++x){
   auto sample=[&](uint32_t channel){const std::size_t offset=(std::size_t(x)*samples+channel)*(bits/8);if(bits==8)return source[offset];std::uint16_t value=0;std::memcpy(&value,source+offset,2);return static_cast<std::uint8_t>(value>>8);};
   auto* destination=rgb.data()+std::size_t(x)*3;
   if(photo==PHOTOMETRIC_RGB){for(int channel=0;channel<3;++channel)destination[channel]=sample(channel);}
   else{auto value=sample(0);if(photo==PHOTOMETRIC_MINISWHITE)value=255-value;destination[0]=destination[1]=destination[2]=value;}
  }
 };
 if(!TIFFIsTiled(tiff)){
  const auto bytes=TIFFScanlineSize(tiff);
  if(bytes<0||static_cast<std::uint64_t>(bytes)<checked_multiply(checked_multiply(width,samples),bits/8)){TIFFClose(tiff);throw std::runtime_error("TIFF scanline invalida");}
  std::vector<std::uint8_t> scan(checked_size(bytes));
  for(uint32_t y=0;y<height;++y){if(TIFFReadScanline(tiff,scan.data(),y,0)<0){TIFFClose(tiff);throw std::runtime_error("Falha TIFF scanline");}convert(scan.data());row(y,rgb.data());}
 }else{
  uint32_t tile_width=0,tile_height=0;TIFFGetField(tiff,TIFFTAG_TILEWIDTH,&tile_width);TIFFGetField(tiff,TIFFTAG_TILELENGTH,&tile_height);
  if(!tile_width||!tile_height){TIFFClose(tiff);throw std::runtime_error("Tiling TIFF invalido");}
  const auto tile_bytes=checked_multiply(checked_multiply(checked_multiply(tile_width,tile_height),samples),bits/8);
  if(tile_bytes>512ULL*1024*1024){TIFFClose(tiff);throw std::runtime_error("Tile TIFF ultrapassa 512 MiB");}
  std::vector<std::uint8_t> tile(checked_size(tile_bytes));
  std::vector<std::uint8_t> band(checked_size(checked_multiply(checked_multiply(width,tile_height),3)));
  for(uint32_t y0=0;y0<height;y0+=tile_height){
   const auto band_height=std::min(tile_height,height-y0);std::fill(band.begin(),band.end(),0);
   for(uint32_t x0=0;x0<width;x0+=tile_width){
    const auto index=TIFFComputeTile(tiff,x0,y0,0,0);
    if(TIFFReadEncodedTile(tiff,index,tile.data(),static_cast<tmsize_t>(tile.size()))<0){TIFFClose(tiff);throw std::runtime_error("Falha TIFFReadEncodedTile");}
    const auto block_width=std::min(tile_width,width-x0);
    for(uint32_t y=0;y<band_height;++y)for(uint32_t x=0;x<block_width;++x){
     const auto* source=tile.data()+(std::size_t(y)*tile_width+x)*samples*(bits/8);
     auto sample=[&](int channel){if(bits==8)return source[channel];std::uint16_t value=0;std::memcpy(&value,source+channel*2,2);return static_cast<std::uint8_t>(value>>8);};
     auto* destination=band.data()+(std::size_t(y)*width+x0+x)*3;
     if(photo==PHOTOMETRIC_RGB){for(int channel=0;channel<3;++channel)destination[channel]=sample(channel);}
     else{auto value=sample(0);if(photo==PHOTOMETRIC_MINISWHITE)value=255-value;destination[0]=destination[1]=destination[2]=value;}
    }
   }
   for(uint32_t y=0;y<band_height;++y)row(y0+y,band.data()+std::size_t(y)*width*3);
  }
 }
 TIFFClose(tiff);return {width,height,"TIFF",bits==16?"PREVIEW_16_TO_8_HIGH_BYTE":"RGB_8_VIEW"};
}

Dimensions decode(const fs::path& path,const RowCallback& row){
 std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Arquivo de origem ausente");
 std::array<unsigned char,8> signature{};input.read(reinterpret_cast<char*>(signature.data()),signature.size());
 if(input.gcount()<4)throw std::runtime_error("Arquivo muito curto");
 if(signature[0]==0x89&&signature[1]=='P'&&signature[2]=='N'&&signature[3]=='G')return decode_png(path,row);
 if(signature[0]==0xff&&signature[1]==0xd8)return decode_jpeg(path,row);
 const bool tiff=(signature[0]=='I'&&signature[1]=='I'&&(signature[2]==42||signature[2]==43)&&signature[3]==0)||
                 (signature[0]=='M'&&signature[1]=='M'&&signature[2]==0&&(signature[3]==42||signature[3]==43));
 if(tiff)return decode_tiff(path,row);
 throw std::runtime_error("Formato nao suportado: PNG, JPEG, TIFF ou BigTIFF");
}

Dimensions probe(const fs::path& path){
 std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Arquivo nao existe");
 std::array<unsigned char,8> signature{};input.read(reinterpret_cast<char*>(signature.data()),signature.size());
 if(input.gcount()<4)throw std::runtime_error("Arquivo muito curto");
 if(signature[0]==0x89&&signature[1]=='P'&&signature[2]=='N'&&signature[3]=='G'){
  png_image image{};image.version=PNG_IMAGE_VERSION;if(!png_image_begin_read_from_file(&image,path.c_str()))throw std::runtime_error("PNG header invalido");
  Dimensions result{image.width,image.height,"PNG","RGB_8_VIEW"};png_image_free(&image);return result;
 }
 if(signature[0]==0xff&&signature[1]==0xd8){
  FILE* file=std::fopen(path.c_str(),"rb");if(!file)throw std::runtime_error("JPEG inacessivel");
  jpeg_decompress_struct state{};JpegError error{};state.err=jpeg_std_error(&error.pub);error.pub.error_exit=tiles_jpeg_failed;
  if(setjmp(error.jump)){jpeg_destroy_decompress(&state);std::fclose(file);throw std::runtime_error("JPEG header invalido");}
  jpeg_create_decompress(&state);jpeg_stdio_src(&state,file);jpeg_read_header(&state,TRUE);
  Dimensions result{state.image_width,state.image_height,"JPEG","RGB_8_VIEW"};jpeg_destroy_decompress(&state);std::fclose(file);return result;
 }
 TIFF* tiff=TIFFOpen(path.c_str(),"r");if(!tiff)throw std::runtime_error("Imagem nao reconhecida");uint32_t width=0,height=0;
 TIFFGetField(tiff,TIFFTAG_IMAGEWIDTH,&width);TIFFGetField(tiff,TIFFTAG_IMAGELENGTH,&height);TIFFClose(tiff);return {width,height,"TIFF","RGB_8_VIEW"};
}

} // namespace

Pyramid create(const fs::path& source,const fs::path& directory,std::uint32_t tile){
 if(tile<64||tile>1024||(tile&(tile-1)))throw std::runtime_error("Tile deve ser potencia de 2 entre 64 e 1024");
 if(!fs::is_regular_file(source))throw std::runtime_error("Fonte nao e arquivo regular");
 const auto dimensions=probe(source);if(!dimensions.width||!dimensions.height)throw std::runtime_error("Dimensoes vazias");
 if(dimensions.width>std::numeric_limits<std::uint32_t>::max()||dimensions.height>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("Limite do codec: 32 bits por eixo");
 fs::create_directories(directory/"L0");
 Pyramid pyramid{dimensions.width,dimensions.height,tile,sha256_file(source),dimensions.format,dimensions.transform,{{dimensions.width,dimensions.height}}};
 const auto columns=divceil(dimensions.width,tile);
 const auto scratch=directory/".rowband.tmp";
 struct TempGuard {fs::path path;~TempGuard(){std::error_code ignored;fs::remove(path,ignored);}} guard{scratch};
 std::fstream band;std::uint64_t current_band=std::numeric_limits<std::uint64_t>::max();
 auto start_band=[&](std::uint64_t index){band.close();band.clear();band.open(scratch,std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);if(!band)throw std::runtime_error("Nao foi possivel criar banda temporaria");current_band=index;};
 auto flush_band=[&](std::uint64_t band_y,std::uint32_t band_height){
  band.flush();if(!band)throw std::runtime_error("Falha ao gravar banda temporaria");
  for(std::uint64_t tile_x=0;tile_x<columns;++tile_x){
   const auto tile_width=static_cast<std::uint32_t>(std::min<std::uint64_t>(tile,dimensions.width-tile_x*tile));
   std::vector<std::uint8_t> rgb(checked_size(checked_multiply(checked_multiply(tile_width,band_height),3)));
   for(std::uint32_t y=0;y<band_height;++y){
    const auto offset=checked_multiply(y,dimensions.width)+tile_x*tile;band.seekg(static_cast<std::streamoff>(checked_multiply(offset,3)),std::ios::beg);
    band.read(reinterpret_cast<char*>(rgb.data()+std::size_t(y)*tile_width*3),tile_width*3);if(!band)throw std::runtime_error("Falha ao ler banda temporaria");
   }
   store_png(directory/"L0"/tile_name(tile_x,band_y),rgb.data(),tile_width,band_height);
  }
  band.close();band.clear();
 };
 const auto actual=decode(source,[&](std::uint64_t y,const std::uint8_t* scanline){
  const auto band_y=y/tile;if(band_y!=current_band)start_band(band_y);
  band.write(reinterpret_cast<const char*>(scanline),static_cast<std::streamsize>(checked_multiply(dimensions.width,3)));if(!band)throw std::runtime_error("Disco insuficiente para banda temporaria");
  if(y%tile==tile-1||y==dimensions.height-1)flush_band(band_y,static_cast<std::uint32_t>(y-band_y*tile+1));
 });
 if(actual.width!=dimensions.width||actual.height!=dimensions.height)throw std::runtime_error("Arquivo mudou durante a leitura");
 pyramid.preview_transform=actual.transform;
 while(pyramid.levels.back().width>tile||pyramid.levels.back().height>tile){
  const auto previous=pyramid.levels.back();const Level next{divceil(previous.width,2),divceil(previous.height,2)};
  const auto old_index=pyramid.levels.size()-1,new_index=pyramid.levels.size();
  const auto input=directory/("L"+std::to_string(old_index)),output=directory/("L"+std::to_string(new_index));fs::create_directories(output);
  const auto columns_out=divceil(next.width,tile),rows_out=divceil(next.height,tile);
  for(std::uint64_t tile_y=0;tile_y<rows_out;++tile_y)for(std::uint64_t tile_x=0;tile_x<columns_out;++tile_x){
   const auto width=static_cast<std::uint32_t>(std::min<std::uint64_t>(tile,next.width-tile_x*tile));
   const auto height=static_cast<std::uint32_t>(std::min<std::uint64_t>(tile,next.height-tile_y*tile));
   std::array<SmallImage,4> children{};const auto previous_columns=divceil(previous.width,tile),previous_rows=divceil(previous.height,tile);
   for(unsigned child_y=0;child_y<2;++child_y)for(unsigned child_x=0;child_x<2;++child_x){const auto x=tile_x*2+child_x,y=tile_y*2+child_y;if(x<previous_columns&&y<previous_rows)children[child_y*2+child_x]=load_tile(input/tile_name(x,y),tile);}
   std::vector<std::uint8_t> rgb(std::size_t(width)*height*3);
   auto pixel=[&](std::uint64_t x,std::uint64_t y,int channel){const auto& child=children[(y/tile-tile_y*2)*2+(x/tile-tile_x*2)];return child.rgb[(std::size_t(y%tile)*child.width+x%tile)*3+channel];};
   for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){const auto source_x=(tile_x*tile+x)*2,source_y=(tile_y*tile+y)*2;for(int channel=0;channel<3;++channel){unsigned int sum=0,count=0;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)if(source_x+dx<previous.width&&source_y+dy<previous.height){sum+=pixel(source_x+dx,source_y+dy,channel);++count;}rgb[(std::size_t(y)*width+x)*3+channel]=static_cast<std::uint8_t>((sum+count/2)/count);}}
   store_png(output/tile_name(tile_x,tile_y),rgb.data(),width,height);
  }
  pyramid.levels.push_back(next);
 }
 return pyramid;
}

} // namespace rizoma::tiles
