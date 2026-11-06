// Synthetic JPEG XL DNGs. All pixel values and metadata are authored here.
// No photograph is converted, downloaded or checked into Git.
#pragma once
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <fstream>
#include <functional>

namespace jxl_checks {
struct Fixture {
  bool jxl=true, floating=false, unequal=false, opcode2=false, opcode3=false, active=false, lossy=false;
  uint16_t orientation=1;
  uint32_t cropWidth=44,cropHeight=34;
  bool trimOpcode1=false;
};
static void require(bool ok,const std::string& message) {
  if(!ok)throw std::runtime_error(message);
}
static std::vector<uint8_t> encode(const Fixture& f) {
  auto encoder=JxlEncoderMake(nullptr);
  JxlBasicInfo info;JxlEncoderInitBasicInfo(&info);
  info.xsize=64;info.ysize=48;info.num_color_channels=3;
  info.bits_per_sample=16;info.exponent_bits_per_sample=f.floating?5:0;
  info.uses_original_profile=f.lossy?JXL_FALSE:JXL_TRUE;
  require(JxlEncoderSetBasicInfo(encoder.get(),&info)==JXL_ENC_SUCCESS,"JXL basic info");
  JxlColorEncoding color;JxlColorEncodingSetToLinearSRGB(&color,JXL_FALSE);
  require(JxlEncoderSetColorEncoding(encoder.get(),&color)==JXL_ENC_SUCCESS,"JXL linear profile");
  auto* settings=JxlEncoderFrameSettingsCreate(encoder.get(),nullptr);
  require(JxlEncoderSetFrameLossless(settings,f.lossy?JXL_FALSE:JXL_TRUE)==JXL_ENC_SUCCESS,"JXL compression mode");
  if(f.lossy)require(JxlEncoderSetFrameDistance(settings,.01f)==JXL_ENC_SUCCESS,"JXL lossy distance");
  std::vector<uint16_t> integer(64*48*3);
  std::vector<float> floats(integer.size());
  for(int y=0;y<48;++y)for(int x=0;x<64;++x)for(int c=0;c<3;++c) {
    // A spatial ramp detects crop offsets; all samples stay well below white.
    const int sample=x==63&&y==47?3000:512+x*4+y*2;
    integer[(y*64+x)*3+c]=uint16_t(sample*(f.unequal?(1<<c):1));
    floats[(y*64+x)*3+c]=.125f+c*.0625f;
  }
  if(!f.jxl) {
    std::vector<uint8_t> bytes;
    for(uint16_t v:integer){bytes.push_back(uint8_t(v));bytes.push_back(uint8_t(v>>8));}
    return bytes;
  }
  JxlPixelFormat format{3,f.floating?JXL_TYPE_FLOAT:JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};
  require(JxlEncoderAddImageFrame(settings,&format,f.floating?static_cast<void*>(floats.data()):static_cast<void*>(integer.data()),f.floating?floats.size()*4:integer.size()*2)==JXL_ENC_SUCCESS,"JXL frame");
  JxlEncoderCloseInput(encoder.get());
  std::vector<uint8_t> bytes(65536);uint8_t* next=bytes.data();size_t available=bytes.size();
  require(JxlEncoderProcessOutput(encoder.get(),&next,&available)==JXL_ENC_SUCCESS,"small JXL output fits buffer");
  bytes.resize(bytes.size()-available);return bytes;
}
static std::vector<uint8_t> dng(const Fixture& f) {
  struct Tag {uint16_t id,type;uint32_t count;std::vector<uint8_t> data;};
  auto le=[](std::initializer_list<uint32_t> values,int n){std::vector<uint8_t>b;for(auto v:values)for(int i=0;i<n;++i)b.push_back(uint8_t(v>>(8*i)));return b;};
  auto opcode=[](double slope){
    std::vector<uint8_t> b;
    auto be=[&](uint32_t v){for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(8*i)));};
    auto number=[&](double v){uint64_t bits;std::memcpy(&bits,&v,8);for(int i=7;i>=0;--i)b.push_back(uint8_t(bits>>(8*i)));};
    be(1);be(8);be(0x01030000);be(0);be(52); // one required MapPolynomial
    for(auto v:{0u,0u,48u,64u,0u,3u,1u,1u})be(v); // AreaSpec, all RGB
    be(1);number(0);number(slope); // degree 1: y = slope*x
    return b;
  };
  std::vector<Tag> tags;
  auto tag=[&](uint16_t id,uint16_t type,uint32_t count,std::vector<uint8_t>b){tags.push_back({id,type,count,std::move(b)});};
  auto image=encode(f);
  tag(254,4,1,le({0},4));tag(256,4,1,le({64},4));tag(257,4,1,le({48},4));
  tag(258,3,3,le({16,16,16},2));tag(259,3,1,le({f.jxl?52546u:1u},2));
  tag(262,3,1,le({34892},2));tag(273,4,1,le({0},4));tag(274,3,1,le({f.orientation},2));
  tag(277,3,1,le({3},2));tag(278,4,1,le({48},4));tag(279,4,1,le({uint32_t(image.size())},4));tag(284,3,1,le({1},2));
  tag(339,3,3,le({f.floating?3u:1u,f.floating?3u:1u,f.floating?3u:1u},2));
  tag(50706,1,4,{1,7,1,0});tag(50707,1,4,{1,3,0,0});
  const std::string model="Synthetic JXL regression";auto name=std::vector<uint8_t>(model.begin(),model.end());name.push_back(0);tag(50708,2,name.size(),name);
  tag(50714,4,1,le({0},4));tag(50717,4,3,le({4095,f.unequal?8191u:4095u,f.unequal?16383u:4095u},4));
  tag(50719,4,2,le({3,2},4));tag(50720,4,2,le({f.cropWidth,f.cropHeight},4));
  tag(50721,10,9,le({1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1},4));
  tag(50728,5,3,le({1,1,1,1,1,1},4));tag(50778,3,1,le({21},2));
  if(f.active)tag(50829,4,4,le({4,6,44,58},4));
  if(f.trimOpcode1){
    std::vector<uint8_t> b;
    for(uint32_t v:{1u,6u,0x01030000u,0u,16u,6u,8u,42u,56u})
      for(int i=3;i>=0;--i)b.push_back(uint8_t(v>>(8*i)));
    auto n=b.size();tag(51008,7,n,std::move(b));
  }
  if(f.opcode2){auto b=opcode(.5);auto n=b.size();tag(51009,7,n,std::move(b));}
  if(f.opcode3){auto b=opcode(.25);auto n=b.size();tag(51022,7,n,std::move(b));}
  std::sort(tags.begin(),tags.end(),[](const auto&a,const auto&b){return a.id<b.id;});
  std::vector<uint8_t> bytes(8+2+12*tags.size()+4,0);bytes[0]=bytes[1]='I';bytes[2]=42;bytes[4]=8;
  auto put=[&](size_t pos,uint32_t v,int n){for(int i=0;i<n;++i)bytes.at(pos+i)=uint8_t(v>>(8*i));};
  put(8,tags.size(),2);size_t strip=0;
  for(size_t i=0;i<tags.size();++i){const auto&t=tags[i];size_t p=10+i*12;put(p,t.id,2);put(p+2,t.type,2);put(p+4,t.count,4);if(t.id==273)strip=p+8;
    if(t.data.size()<=4)std::copy(t.data.begin(),t.data.end(),bytes.begin()+p+8);
    else{put(p+8,bytes.size(),4);bytes.insert(bytes.end(),t.data.begin(),t.data.end());if(bytes.size()%2)bytes.push_back(0);}}
  put(strip,bytes.size(),4);bytes.insert(bytes.end(),image.begin(),image.end());return bytes;
}
struct TemporaryFixtures {
  std::filesystem::path directory;
  TemporaryFixtures(){
    auto stamp=Clock::now().time_since_epoch().count();
    for(int n=0;n<100;++n){directory=std::filesystem::temp_directory_path()/("rawnode-jxl-"+std::to_string(stamp)+"-"+std::to_string(n));if(std::filesystem::create_directory(directory))return;}
    throw std::runtime_error("Cannot create temporary fixture directory");
  }
  ~TemporaryFixtures(){std::error_code e;std::filesystem::remove_all(directory,e);}
  std::string write(const std::string& name,const Fixture& fixture){auto path=directory/(name+".dng");auto bytes=dng(fixture);std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(file),"Write synthetic fixture");return path.string();}
};
static int sampleAt(int x,int y) {return x==63&&y==47?3000:512+x*4+y*2;}
static void pixels(const Result& result,const Fixture& f,bool crop) {
  const int width=crop?int(f.cropWidth):(f.active?52:64), height=crop?int(f.cropHeight):(f.active?40:48);
  const int left=(f.active?6:0)+(crop?3:0), top=(f.active?4:0)+(crop?2:0);
  const bool rotated=f.orientation==6;
  require(result.w==(rotated?height:width)&&result.h==(rotated?width:height),"Wrong output dimensions/default crop");
  require(result.camera.size()==size_t(result.w*result.h*3),"Missing diagnostic camera pixels");
  double maximum=0;
  for(int y=0;y<result.h;++y)for(int x=0;x<result.w;++x)for(int c=0;c<3;++c){
    const int sx=left+(rotated?y:x),sy=top+(rotated?height-1-x:y);
    const double white=f.unequal?(c==0?4095:c==1?8191:16383):4095;
    // Independent source ramp, white-level normalization and polynomial y=.5*x.
    const double expected=sampleAt(sx,sy)*(f.unequal?(1<<c):1)/white*(f.opcode2?.5:1.);
    maximum=std::max(maximum,std::abs(result.camera[(y*result.w+x)*3+c]-expected));
  }
  const double tolerance=f.lossy?.01:4./65535;
  std::cout<<"ORACLE max_camera_error="<<maximum<<" tolerance="<<tolerance<<'\n';
  require(maximum<=tolerance,"Independent normalized/polynomial pixel oracle failed");
}
static void run() {
  TemporaryFixtures temporary;
  int passed=0;
  auto test=[&](const std::string& name,const std::function<void()>& action){
    action();++passed;std::cout<<"PASS JXL "<<name<<'\n';
  };
  auto decode=[&](const std::string& name,const Fixture& f,bool crop=false){
    Options o;o.captureCamera=true;o.crop=crop;
    auto result=loadOriginal(temporary.write(name,f),o);
    require(result.sdkProcessed==f.jxl,"Wrong SDK dispatch");
    require(!result.stage3,"Stage 3 was automatically executed");
    require(result.stage2==(f.jxl&&f.opcode2),"Wrong Stage 2 selection");
    pixels(result,f,crop);return result;
  };
  test("integer decompression",[&]{decode("integer",{});});
  test("lossy XYB linear RGB decompression",[&]{Fixture f;f.lossy=true;decode("lossy",f);});
  test("OpcodeList2",[&]{Fixture f;f.opcode2=true;decode("opcode2",f);});
  test("unequal whites and OpcodeList2",[&]{Fixture f;f.unequal=f.opcode2=true;decode("unequal-op2",f);});
  test("unequal whites without OpcodeList2",[&]{Fixture f;f.unequal=true;decode("unequal",f);});
  test("OpcodeList3 only matches ordinary DNG",[&]{
    Fixture f;f.opcode3=true;auto a=decode("op3",f);f.jxl=false;auto b=decode("op3-ordinary",f);
    require(a.camera==b.camera,"OpcodeList3 differs from ordinary uncompressed development");
  });
  test("OpcodeList2 and 3 selects only Stage 2",[&]{Fixture f;f.opcode2=f.opcode3=true;decode("op2-and-3",f);});
  test("ActiveArea without OpcodeList2",[&]{Fixture f;f.active=true;decode("active",f);});
  test("ActiveArea with OpcodeList2",[&]{Fixture f;f.active=f.opcode2=true;decode("active-op2",f);});
  test("DefaultCrop preserved after Stage 2 trim",[&]{Fixture f;f.active=f.opcode2=true;decode("active-crop",f,true);});
  test("crop without Stage 2",[&]{Fixture f;f.active=true;decode("active-crop-no-op2",f,true);});
  test("small DefaultCrop is not silently skipped",[&]{Fixture f;f.active=f.opcode2=true;f.cropWidth=20;f.cropHeight=12;decode("small-crop",f,true);});
  test("rotated DefaultCrop after Stage 2",[&]{Fixture f;f.active=f.opcode2=true;f.orientation=6;decode("rotated-crop",f,true);});
  test("unmapped opcode geometry is rejected",[&]{
    Fixture f;f.opcode2=f.trimOpcode1=true;bool rejected=false;
    try{decode("trim-opcode1",f);}catch(const std::exception&e){rejected=std::string(e.what()).find("Unsupported DNG Stage 1 TrimBounds")!=std::string::npos;}
    require(rejected,"Unexpected geometry was accepted or failed without a crop diagnostic");
  });
  test("default SDK eligibility is unchanged",[&]{
    LibRaw raw;const auto mask=raw.imgdata.rawparams.use_dngsdk;Options o;configure(raw,o,true,true);
    require(raw.imgdata.rawparams.use_dngsdk==mask,"Changed default SDK eligibility mask");
    require((raw.imgdata.rawparams.options&LIBRAW_RAWOPTIONS_DNG_STAGE2)!=0,"Missing forced Stage 2 before open");
  });
  for(bool withOpcode:{false,true})test(withOpcode?"float with OpcodeList2 rejected":"float rejected",[&]{
    Fixture f;f.floating=true;f.opcode2=withOpcode;const auto path=temporary.write(withOpcode?"float-op2":"float",f);
    bool rejected=false;
    try{loadOriginal(path,Options{});}catch(const std::exception&e){rejected=std::string(e.what()).find("Unsupported floating-point JPEG XL DNG")!=std::string::npos;}
    require(rejected,"Float was silently developed or wrong diagnostic");
    Options diagnostic;diagnostic.keepFloat=true;diagnostic.captureCamera=true;
    const auto retained=loadOriginal(path,diagnostic);
    require(retained.sdkProcessed&&retained.retainedFloat,"Lost float diagnostic path");
    require(retained.camera.size()==64*48*3,"Missing diagnostic float samples");
    for(size_t i=0;i<retained.camera.size();++i){
      const double expected=(.125+(i%3)*.0625)*(withOpcode?.5/4095:1.);
      require(std::abs(retained.camera[i]-expected)<1e-9,"Retained float sample does not match analytical normalization");
    }
  });
  test("missing SDK host is an explicit failure",[&]{
    const auto path=temporary.write("no-host",Fixture{});Options o;o.sdk=false;bool failed=false;
    try{loadOriginal(path,o);}catch(const std::exception&e){failed=std::string(e.what()).find("unpack:")!=std::string::npos;}
    require(failed,"Missing-host JXL input was accepted");
  });
  std::cout<<"PASS "<<passed<<" real JPEG XL regressions; synthetic files removed on exit\n";
}
} // namespace jxl_checks
