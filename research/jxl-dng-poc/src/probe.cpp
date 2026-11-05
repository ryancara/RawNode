// Standalone experiment: reads source images; never writes image files.
#define LIBRAW_DNGSDK_CONFLICT 1
#include <libraw/libraw.h>
#if POC_WITH_DNGSDK
#include "dng_host.h"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

static_assert(LIBRAW_VERSION == LIBRAW_MAKE_VERSION(0,22,3), "Use LibRaw 0.22.3 headers");
struct Options {
  std::string stage = "default";
  bool sdk = true, keepFloat = false, allowSize = false, crop = false, reference = false, compareStage = false;
  int highlight = 0;
};
struct Result {
  int w=0,h=0;
  std::vector<float> pixels;
  std::array<float,3> cameraMiddle{};
  unsigned unpackedMiddle=0;
};
using Clock = std::chrono::steady_clock;
static void check(int code, const char* operation) {
  if (code != LIBRAW_SUCCESS)
    throw std::runtime_error(std::string(operation)+": "+LibRaw::strerror(code)+" ("+std::to_string(code)+")");
}
static bool isJxlDng(LibRaw& raw) {
  libraw_decoder_info_t info{};
  check(raw.get_decoder_info(&info), "get_decoder_info");
  // Public diagnostic API, but decoder name is specific to pinned LibRaw 0.22.3.
  // Never retry every unsupported file or classify by filename extension.
  return raw.imgdata.idata.dng_version && info.decoder_name &&
    std::strcmp(info.decoder_name,"jxl_dng_load_raw_placeholder()") == 0;
}
static void configure(LibRaw& raw, const Options& o, bool jxl) {
  // Stage options affect identify-time WB metadata adjustment as well as unpack.
  // Apply them before open_file, and only to an already classified JXL request.
  if(jxl) {
    if(o.stage=="opcode") raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_DNG_STAGE23_IFPRESENT_JPGJXL;
    else if(o.stage=="stage2") raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_DNG_STAGE2;
    else if(o.stage=="stage3") raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_DNG_STAGE3;
    if(o.allowSize) raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_DNG_ALLOWSIZECHANGE;
  }
  if(o.keepFloat) raw.imgdata.rawparams.options &= ~LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT;
}
static void metadata(LibRaw& raw, const char* phase) {
  const auto& d=raw.imgdata;
  const auto& s=d.sizes;
  const auto& c=d.color;
  std::cout << "phase=" << phase << " raw=" << s.raw_width << 'x' << s.raw_height
    << " active=" << s.width << 'x' << s.height << " margins=" << s.left_margin << ',' << s.top_margin
    << " flip=" << s.flip << " filters=" << d.idata.filters << " channels=" << d.idata.colors
    << " black=" << c.black << " maximum=" << c.maximum << " warnings=" << d.process_warnings
    << " float_source=" << raw.is_floating_point() << '\n';
  std::cout << "cam_mul=";
  for(float x:c.cam_mul) std::cout << x << ',';
  std::cout << " pre_mul=";
  for(float x:c.pre_mul) std::cout << x << ',';
  std::cout << " dng_white=";
  for(auto x:c.dng_levels.dng_whitelevel) std::cout << x << ',';
  std::cout << " baseline_exposure=" << c.dng_levels.baseline_exposure << '\n';
  std::cout << "cblack=" << c.cblack[0] << ',' << c.cblack[1] << ',' << c.cblack[2]
    << " curve=" << c.curve[0] << ',' << c.curve[256] << ',' << c.curve[1024] << ',' << c.curve[65535] << '\n';
  for(int i=0;i<3;++i) {
    std::cout << "rgb_cam[" << i << "]=";
    for(float x:c.rgb_cam[i]) std::cout << x << ',';
    std::cout << " opcode" << i+1 << "_bytes=" << c.dng_levels.rawopcodes[i].len << '\n';
    const auto& ops=c.dng_levels.rawopcodes[i];
    const auto* bytes=static_cast<const unsigned char*>(ops.data);
    if(bytes && ops.len>=4) {
      auto be=[&](size_t p){return (uint32_t(bytes[p])<<24)|(uint32_t(bytes[p+1])<<16)|(uint32_t(bytes[p+2])<<8)|bytes[p+3];};
      const uint32_t count=be(0);size_t pos=4;
      for(uint32_t op=0;op<count && pos+16<=ops.len;++op) {
        const auto length=be(pos+12);if(length>ops.len-pos-16)break;
        std::cout << "opcode_list=" << i+1 << " id=" << be(pos) << " flags=" << be(pos+8) << " payload_bytes=" << length << '\n';
        pos+=16+length;
      }
    }
  }
  for(const auto& crop:s.raw_inset_crops)
    std::cout << "inset=" << crop.cleft << ',' << crop.ctop << ',' << crop.cwidth << ',' << crop.cheight << '\n';
}
static void statistics(const std::vector<float>& pixels, int channels, const char* label) {
  uint64_t fingerprint=14695981039346656037ULL;
  for(float value:pixels) {
    uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));
    for(int b=0;b<4;++b){fingerprint^=(bits>>(8*b))&255;fingerprint*=1099511628211ULL;}
  }
  std::cout << label << " fingerprint_fnv64=" << fingerprint << '\n';
  for(int c=0;c<channels;++c) {
    double sum=0; float lo=INFINITY,hi=-INFINITY;
    size_t below=0,above=0,zeros=0,ones=0,nonfinite=0,count=0;
    for(size_t i=c;i<pixels.size();i+=channels) {
      float v=pixels[i];
      if(!std::isfinite(v)) { ++nonfinite; continue; }
      lo=std::min(lo,v); hi=std::max(hi,v); sum+=v; ++count;
      below+=v<0; above+=v>1; zeros+=v==0; ones+=v==1;
    }
    std::cout << label << " channel=" << c << " min=" << lo << " max=" << hi
      << " mean=" << (count?sum/count:0) << " below0=" << below << " above1=" << above
      << " zero=" << zeros << " one=" << ones << " nonfinite=" << nonfinite << '\n';
  }
}
#if POC_WITH_DNGSDK
#include "reference.h"
static_assert(kDNGSDK_BuildVersion==2768,"Use the uploaded official SDK build 2768");
#endif
static Result process(LibRaw& raw, const Options& o, const std::string& path={}) {
  const auto begin=Clock::now();
  metadata(raw,"identified");
  const bool jxl=isJxlDng(raw);
  bool attached=false;
#if POC_WITH_DNGSDK
  // Host declared after caller's raw: detach and recycle before host destruction.
  // This function owns all subsequent raw/SDK buffer use.
  dng_host host;
  struct HostGuard {
    LibRaw& raw;
    ~HostGuard() { raw.recycle(); raw.set_dng_host(nullptr); }
  } guard{raw};
  if(jxl && o.sdk) { raw.set_dng_host(&host); attached=true; }
#endif
  if((o.stage!="default" || o.allowSize) && !attached)
    throw std::runtime_error("SDK stage experiments require classified JXL input and an SDK-enabled build/host");
  if(o.crop) std::cout << "crop_selection=" << raw.adjust_to_raw_inset_crop(1) << " (0=none,1=first inset)\n";
  std::cout << "jxl_selected=" << jxl << " host_attached=" << attached
    << " stage=" << o.stage << " options=" << raw.imgdata.rawparams.options << '\n';
  const auto unpackStart=Clock::now();
  int error=raw.unpack(); metadata(raw,error?"unpack_failed_state_may_be_recycled":"unpacked"); check(error,"unpack");
  if(jxl && attached && !(raw.imgdata.process_warnings & LIBRAW_WARN_DNGSDK_PROCESSED))
    throw std::runtime_error("JXL unpack returned success without SDK_PROCESSED evidence");
  const auto unpackEnd=Clock::now();
  std::cout << "unpack_ms=" << std::chrono::duration<double,std::milli>(unpackEnd-unpackStart).count()
    << " sdk_processed=" << bool(raw.imgdata.process_warnings & LIBRAW_WARN_DNGSDK_PROCESSED)
    << " stage2_applied=" << bool(raw.imgdata.process_warnings & LIBRAW_WARN_DNG_STAGE2_APPLIED)
    << " stage3_applied=" << bool(raw.imgdata.process_warnings & LIBRAW_WARN_DNG_STAGE3_APPLIED) << '\n';
  const auto& rd=raw.imgdata.rawdata;
  unsigned unpackedMiddle=rd.color3_image ? rd.color3_image[raw.imgdata.sizes.raw_width/2][1] :
    rd.color4_image ? rd.color4_image[raw.imgdata.sizes.raw_width/2][1] : 0;
  std::cout << "unpacked_buffers u16_rgb=" << bool(rd.color3_image)
    << " u16_rgba=" << bool(rd.color4_image) << " float=" << bool(rd.float_image)
    << " float_rgb=" << bool(rd.float3_image) << " float_rgba=" << bool(rd.float4_image) << '\n';
#if POC_WITH_DNGSDK
  if(o.reference && attached)compareSdkReference(raw,host,path);
#endif
  if(o.keepFloat) {
    if(!rd.float_image && !rd.float3_image && !rd.float4_image)
      throw std::runtime_error("--keep-float requested but no float unpack buffer exists");
    // Do not pass an unsupported float buffer into the integer dcraw pipeline.
    const float* data=rd.float_image ? rd.float_image : rd.float3_image ? &rd.float3_image[0][0] : &rd.float4_image[0][0];
    const int channels=rd.float_image?1:rd.float3_image?3:4;
    const size_t count=size_t(raw.imgdata.sizes.raw_width)*raw.imgdata.sizes.raw_height*channels;
    std::vector<float> copy(data,data+count); statistics(copy,channels,"unpacked_float");
    return {};
  }
  // Match RawNode's current development settings; highlight is an explicit experiment.
  auto& p=raw.imgdata.params;
  p.output_bps=16; p.gamm[0]=p.gamm[1]=1; p.no_auto_bright=1;
  p.use_camera_wb=1; p.output_color=0; p.highlight=o.highlight;
  check(raw.dcraw_process(),"dcraw_process"); metadata(raw,"processed");
  int memError=0;
  std::unique_ptr<libraw_processed_image_t,void(*)(libraw_processed_image_t*)>
    img(raw.dcraw_make_mem_image(&memError),LibRaw::dcraw_clear_mem);
  check(memError,"dcraw_make_mem_image");
  if(!img || img->type!=LIBRAW_IMAGE_BITMAP || img->colors<3 || img->colors>4 || img->bits!=16)
    throw std::runtime_error("Unexpected processed bitmap format");
  Result result;
  result.w=img->width; result.h=img->height; result.unpackedMiddle=unpackedMiddle;
  for(int c=0;c<3;++c) result.cameraMiddle[c]=reinterpret_cast<const uint16_t*>(img->data)[size_t(img->width/2)*img->colors+c]/65535.f;
  const size_t n=size_t(img->width)*img->height;
  if(img->data_size<n*img->colors*2) throw std::runtime_error("Short bitmap buffer");
  result.pixels.resize(n*4);
  std::vector<float> camera(n*img->colors);
  const auto* src=reinterpret_cast<const uint16_t*>(img->data);
  float matrix[3][4]{}; bool haveMatrix=false;
  for(int r=0;r<3;++r) for(int c=0;c<4;++c) { matrix[r][c]=raw.imgdata.color.rgb_cam[r][c]; haveMatrix |= matrix[r][c]!=0; }
  if(!haveMatrix) for(int c=0;c<3;++c) matrix[c][c]=1;
  // Fixed standard linear Rec.709 -> Rec.2020 matrix, corresponding to RawNode's default gamut.
  constexpr double to2020[3][3]={{.627403895934699,.329283038377883,.043313065687418},
    {.069097289358232,.919540395075459,.011362315566309},
    {.016391438875150,.088013307877226,.895595253247624}};
  float composed[3][4]{};
  for(int r=0;r<3;++r) for(int c=0;c<4;++c) {
    double v=0;for(int k=0;k<3;++k)v+=to2020[r][k]*matrix[k][c];composed[r][c]=float(v);
  }
  constexpr float scale=1.f/65535.f;
  for(int y=0;y<img->height;++y) for(int x=0;x<img->width;++x) {
    size_t si=(size_t(img->height-1-y)*img->width+x)*img->colors;
    size_t di=(size_t(y)*img->width+x)*4;
    for(int r=0;r<3;++r) {
      float v=0;for(int c=0;c<img->colors;++c)v+=composed[r][c]*(src[si+c]*scale);
      result.pixels[di+r]=v;
    }
    result.pixels[di+3]=1;
  }
  for(size_t i=0;i<camera.size();++i) camera[i]=src[i]/65535.f;
  statistics(camera,img->colors,"camera_processed"); statistics(result.pixels,4,"linear_rec2020");
  const auto end=Clock::now();
  std::cout << "bitmap=" << img->width << 'x' << img->height << " channels=" << img->colors
    << " bits=" << img->bits << " float_rgba_bytes=" << result.pixels.size()*sizeof(float)
    << " unpack_ms=" << std::chrono::duration<double,std::milli>(unpackEnd-unpackStart).count()
    << " total_after_identify_ms=" << std::chrono::duration<double,std::milli>(end-begin).count() << '\n';
  return result;
}

// Minimal classic TIFF/DNG memory fixture builder. No fixture is written to disk.
static std::vector<uint8_t> synthetic(uint16_t compression=1, uint16_t orientation=1, bool table=false, uint16_t black=0) {
  struct Tag { uint16_t id,type; uint32_t count; std::vector<uint8_t> data; };
  auto bytes=[](std::initializer_list<uint32_t> values,int width) {
    std::vector<uint8_t> b; for(auto v:values) for(int i=0;i<width;++i) b.push_back(uint8_t(v>>(i*8))); return b;
  };
  std::vector<Tag> tags;
  auto tag=[&](uint16_t id,uint16_t type,uint32_t count,std::vector<uint8_t> data){ tags.push_back({id,type,count,std::move(data)}); };
  const uint32_t w=64,h=48;
  tag(254,4,1,bytes({0},4)); tag(256,4,1,bytes({w},4)); tag(257,4,1,bytes({h},4));
  tag(258,3,3,bytes({16,16,16},2)); tag(259,3,1,bytes({compression},2));
  tag(262,3,1,bytes({34892},2)); tag(273,4,1,bytes({0},4));
  tag(274,3,1,bytes({orientation},2)); tag(277,3,1,bytes({3},2));
  tag(278,4,1,bytes({h},4)); tag(279,4,1,bytes({w*h*6},4)); tag(284,3,1,bytes({1},2));
  tag(339,3,3,bytes({1,1,1},2)); tag(50706,1,4,{1,4,0,0}); tag(50707,1,4,{1,1,0,0});
  const std::string model="RawNode in-memory test";
  auto text=std::vector<uint8_t>(model.begin(),model.end());text.push_back(0); tag(50708,2,text.size(),text);
  tag(50714,4,1,bytes({black},4));tag(50717,4,1,bytes({4095},4));
  tag(50719,4,2,bytes({2,2},4)); tag(50720,4,2,bytes({60,44},4));
  tag(50721,10,9,bytes({1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1},4));
  tag(50728,5,3,bytes({1,2,1,1,1,4},4));tag(50778,3,1,bytes({21},2));
  if(table) {
    std::vector<uint8_t> b;for(uint32_t i=0;i<65536;++i){uint16_t v=uint16_t(std::min(i*2,65535u));b.push_back(v&255);b.push_back(v>>8);}
    tag(50712,3,65536,std::move(b));
  }
  std::sort(tags.begin(),tags.end(),[](auto& a,auto& b){return a.id<b.id;});
  size_t start=8+2+tags.size()*12+4;
  std::vector<uint8_t> result(start,0); result[0]=result[1]='I'; result[2]=42;result[4]=8;
  auto put=[&](size_t pos,uint32_t v,int n){for(int i=0;i<n;++i) result.at(pos+i)=uint8_t(v>>(i*8));};
  put(8,tags.size(),2);size_t stripPointer=0;
  for(size_t i=0;i<tags.size();++i){auto& t=tags[i];size_t p=10+12*i;put(p,t.id,2);put(p+2,t.type,2);put(p+4,t.count,4);
    if(t.id==273) stripPointer=p+8;
    if(t.data.size()<=4) std::copy(t.data.begin(),t.data.end(),result.begin()+p+8);
    else {put(p+8,result.size(),4);result.insert(result.end(),t.data.begin(),t.data.end());if(result.size()%2)result.push_back(0);}
  }
  put(stripPointer,result.size(),4);
  for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) for(int c=0;c<3;++c){
    uint16_t v=uint16_t(x*4095/(w-1));
    if(x==0 && y==0) v=c==0?4095:0; // One red patch exercises negative matrix output.
    result.push_back(v&255);result.push_back(v>>8);
  }
  return result;
}
static void selftest() {
  {
    std::array<uint8_t,32> garbage{};LibRaw raw;
    if(raw.open_buffer(garbage.data(),garbage.size())==LIBRAW_SUCCESS)
      throw std::runtime_error("Malformed input was accepted");
    std::cout << "PASS malformed input rejected without SDK retry\n";
  }
  for(auto [compression,expected]:{std::pair{uint16_t(1),false},std::pair{uint16_t(52546),true}}) {
    auto buffer=synthetic(compression);LibRaw raw;
    check(raw.open_buffer(buffer.data(),buffer.size()),"synthetic identification");
    if(isJxlDng(raw)!=expected) throw std::runtime_error("Dispatch classification failed");
    std::cout << "PASS classification compression=" << compression << " (header only; JXL payload is NOT valid)\n";
  }
  auto run=[](const Options& o,uint16_t orientation=1,bool table=false,uint16_t black=0){
    auto buffer=synthetic(1,orientation,table,black);LibRaw raw;check(raw.open_buffer(buffer.data(),buffer.size()),"synthetic open");return process(raw,o);
  };
  Options defaultOptions;auto a=run(defaultOptions);
  Options headroom;headroom.highlight=1;auto b=run(headroom);
  if(a.w!=64 || a.h!=48 || b.pixels.size()!=a.pixels.size()) throw std::runtime_error("Unexpected dimensions");
  if(run(defaultOptions,6).w!=48) throw std::runtime_error("Orientation not applied");
  Options cropped;cropped.crop=true;auto cropResult=run(cropped);
  if(cropResult.w!=60 || cropResult.h!=44)throw std::runtime_error("Explicit default crop failed");
  const auto extremes=std::minmax_element(a.pixels.begin(),a.pixels.end());
  if(*extremes.first>=0 || *extremes.second<=1)throw std::runtime_error("Float matrix output was clipped");
  std::cout << "PASS default dimensions 64x48, orientation6 48x64, explicit crop 60x44; float_min="
    << *extremes.first << " float_max=" << *extremes.second << '\n';
  auto tableResult=run(defaultOptions,1,true);
  if(a.unpackedMiddle!=2080 || tableResult.unpackedMiddle!=4160)
    throw std::runtime_error("Unpacked table lookup is not exactly once");
  const float input=2080.f/4095.f;
  if(std::fabs(a.cameraMiddle[1]-input)>0.0001f || a.cameraMiddle[2]!=1.f ||
     std::fabs(b.cameraMiddle[2]-input)>0.0001f || b.cameraMiddle[2]*4<2.f)
    throw std::runtime_error("WB scaling/clipping oracle failed");
  auto blackResult=run(defaultOptions,1,false,64);
  const float expectedBlack=(2080.f-64)/(4095-64);
  if(std::fabs(blackResult.cameraMiddle[1]-expectedBlack)>0.0001f)
    throw std::runtime_error("Black/white normalization oracle failed");
  std::cout << "PASS exact unpacked lookup 2080 -> 4160; normalized green=" << a.cameraMiddle[1]
    << "; default blue=" << a.cameraMiddle[2] << "; headroom blue=" << b.cameraMiddle[2]
    << "; headroom blue * 4=" << b.cameraMiddle[2]*4 << "; black64 green=" << blackResult.cameraMiddle[1] << '\n';
  double maxDiff=0;for(size_t i=0;i<a.pixels.size();++i)maxDiff=std::max(maxDiff,double(std::fabs(a.pixels[i]-b.pixels[i])));
  if(maxDiff<0.01)throw std::runtime_error("WB headroom modes did not differ");
  double tableDiff=0;for(size_t i=0;i<a.pixels.size();++i)tableDiff=std::max(tableDiff,double(std::fabs(a.pixels[i]-tableResult.pixels[i])));
  if(tableDiff<0.01)throw std::runtime_error("LinearizationTable had no effect");
  std::cout << "PASS in-memory integer linear DNG, rotation, table response, WB headroom difference=" << maxDiff
    << " table_difference=" << tableDiff << "\n";
}
int main(int argc,char** argv) {
  try {
    std::cout << std::setprecision(9) << "libraw=" << LibRaw::version() << " capabilities=" << LibRaw::capabilities()
      << " sdk_compiled=" << POC_WITH_DNGSDK << '\n';
#if POC_WITH_DNGSDK
    std::cout << "adobe_sdk=" << kDNGSDK_GetInfoVersion << " libjxl_version_number=" << JxlDecoderVersion() << '\n';
#endif
    if(LibRaw::versionNumber()!=LIBRAW_MAKE_VERSION(0,22,3)) throw std::runtime_error("Runtime LibRaw version mismatch");
    if(POC_WITH_DNGSDK && !(LibRaw::capabilities() & LIBRAW_CAPS_DNGSDK))
      throw std::runtime_error("SDK requested but linked LibRaw has no SDK capability");
    if(argc==2 && std::string(argv[1])=="--selftest") { selftest(); return 0; }
    Options o;std::vector<std::string> paths;
    for(int i=1;i<argc;++i){std::string a=argv[i];
      if(a=="--no-sdk")o.sdk=false;
      else if(a=="--keep-float")o.keepFloat=true;
      else if(a=="--reference-unpack")o.reference=true;
      else if(a=="--compare-stage")o.compareStage=true;
      else if(a=="--allow-size-change")o.allowSize=true;
      else if(a=="--crop")o.crop=true;
      else if(a=="--highlight=1")o.highlight=1;
      else if(a.rfind("--stage=",0)==0){o.stage=a.substr(8);if(o.stage!="default"&&o.stage!="opcode"&&o.stage!="stage2"&&o.stage!="stage3")throw std::runtime_error("Unknown stage");}
      else if(a.rfind("--",0)==0)throw std::runtime_error("Unknown option: "+a);
      else paths.push_back(a);
    }
    if(paths.empty()||paths.size()>2)throw std::runtime_error("Usage: dng_probe [--stage=default|opcode|stage2|stage3] [--no-sdk] [--keep-float] [--allow-size-change] [--highlight=1] [--crop] ORIGINAL [COMPARISON]");
    if(o.compareStage){if(paths.size()!=1 || o.stage!="default" || o.keepFloat || o.reference)throw std::runtime_error("--compare-stage needs one input and default processed mode");paths.push_back(paths[0]);}
    std::vector<Result> results;
    for(const auto& path:paths){
      const auto start=Clock::now();
      LibRaw identified;std::cout << "input=" << path << '\n';check(identified.open_file(path.c_str()),"identify open_file");
      const bool jxl=isJxlDng(identified);identified.recycle();
      Options current=o;if(o.compareStage && !results.empty())current.stage="opcode";
      LibRaw raw;configure(raw,current,jxl);check(raw.open_file(path.c_str()),"decode open_file");
      if(isJxlDng(raw)!=jxl)throw std::runtime_error("Input classification changed between opens");
      results.push_back(process(raw,current,path));
      std::cout << "path_total_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << '\n';
    }
    if(results.size()==2){const auto& a=results[0];const auto& b=results[1];
      if(a.pixels.empty()||a.w!=b.w||a.h!=b.h)throw std::runtime_error("Comparison requires equal, nonempty image dimensions; no automatic alignment");
      double sum=0,square=0,maximum=0;size_t count=0;
      for(size_t i=0;i<a.pixels.size();++i)if(i%4!=3){double d=double(a.pixels[i])-b.pixels[i];sum+=std::abs(d);square+=d*d;maximum=std::max(maximum,std::abs(d));++count;}
      std::cout << "comparison RGB MAE=" << sum/count << " RMSE=" << std::sqrt(square/count) << " max_abs=" << maximum
        << (o.compareStage?" (same file; default versus conditional SDK stage processing)\n":" (compression and development differences combined)\n");
    }
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::cout << "peak_rss_bytes=" << uint64_t(usage.ru_maxrss)
#ifndef __APPLE__
      *1024
#endif
      << " (process lifetime, includes comparisons and diagnostics)\n";
#endif
    return 0;
  } catch(const std::exception& e){std::cerr << "ERROR: " << e.what() << '\n';return 1;}
}
