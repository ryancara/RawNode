// Diagnostic comparison against direct SDK decoding of the same original file.
// This validates bridge/stage transfer, not an independent JPEG XL implementation.
#pragma once
#include "dng_file_stream.h"
#include "dng_info.h"
#include "dng_negative.h"
#include "dng_read_image.h"
#include "dng_simple_image.h"
#include "dng_exceptions.h"
#include <jxl/decode.h>

static void compareSdkReference(LibRaw& raw, dng_host& host, const std::string& path) {
  const auto start=Clock::now();
  dng_file_stream stream(path.c_str());
  dng_info info;info.Parse(host,stream);info.PostParse(host);
  if(!info.IsValidDNG())throw std::runtime_error("SDK reference rejected DNG");
  const auto& ifd=*info.fIFD[info.fMainIndex];
  std::cout << "sdk_main_ifd compression=" << ifd.fCompression << " bits_per_sample=" << ifd.fBitsPerSample[0]
    << " sample_format=" << ifd.fSampleFormat[0] << " photometric=" << ifd.fPhotometricInterpretation << '\n';
  AutoPtr<dng_negative> negative;
  AutoPtr<dng_image> direct;
  const dng_image* image=nullptr;
  const bool stage2=(raw.imgdata.process_warnings & LIBRAW_WARN_DNG_STAGE2_APPLIED)!=0;
  const bool stage3=(raw.imgdata.process_warnings & LIBRAW_WARN_DNG_STAGE3_APPLIED)!=0;
  if(stage2) {
    negative.Reset(host.Make_dng_negative());
    negative->Parse(host,stream,info);negative->PostParse(host,stream,info);
    negative->ReadStage1Image(host,stream,info);negative->BuildStage2Image(host);
    if(stage3) { negative->BuildStage3Image(host);image=negative->Stage3Image(); }
    else image=negative->Stage2Image();
  } else {
    direct.Reset(host.Make_dng_image(ifd.Bounds(),ifd.fSamplesPerPixel,ifd.PixelType()));
    dng_read_image reader;reader.Read(host,ifd,stream,*direct.Get(),nullptr,nullptr);
    image=direct.Get();
  }
  const auto& s=raw.imgdata.sizes;
  const auto& rd=raw.imgdata.rawdata;
  const auto* simple=dynamic_cast<const dng_simple_image*>(image);
  if(!simple)throw std::runtime_error("Reference host did not return a simple image");
  // SDK exposes this read-only descriptor accessor without a const qualifier.
  dng_pixel_buffer buffer;const_cast<dng_simple_image*>(simple)->GetPixelBuffer(buffer);
  const bool isFloat=image->PixelType()==ttFloat;
  if((isFloat && !rd.float3_image && !rd.float4_image) ||
     (!isFloat && image->PixelType()!=ttShort))
    throw std::runtime_error("Reference requires short RGB or retained float RGB");
  const int channels=image->Planes();
  if(channels!=3 || image->Bounds().W()!=s.raw_width || image->Bounds().H()!=s.raw_height)
    throw std::runtime_error("Reference geometry/planes mismatch; no automatic alignment");
  double square=0,maximum=0;uint64_t different=0,count=0;
  for(int y=0;y<s.raw_height;++y) for(int x=0;x<s.raw_width;++x) for(int c=0;c<channels;++c) {
    const ptrdiff_t offset=y*ptrdiff_t(buffer.fRowStep)+x*ptrdiff_t(buffer.fColStep)+c*ptrdiff_t(buffer.fPlaneStep);
    const auto* p=static_cast<const unsigned char*>(buffer.fData)+offset*buffer.fPixelSize;
    double expected=0,actual=0;
    const size_t pos=size_t(y)*s.raw_width+x;
    if(isFloat) {
      float value;std::memcpy(&value,p,sizeof(value));expected=value;
      actual=rd.float3_image ? rd.float3_image[pos][c] : rd.float4_image[pos][c];
    } else {
      uint16_t value;std::memcpy(&value,p,sizeof(value));
      expected=stage2?value:raw.imgdata.color.curve[value];
      actual=rd.color3_image ? rd.color3_image[pos][c] : rd.color4_image[pos][c];
    }
    if(!std::isfinite(expected)||!std::isfinite(actual))throw std::runtime_error("Nonfinite reference sample");
    const double delta=actual-expected;square+=delta*delta;maximum=std::max(maximum,std::abs(delta));
    different+=delta!=0;++count;
  }
  std::cout << "sdk_reference samples=" << count << " different=" << different << " max_abs=" << maximum
    << " rmse=" << std::sqrt(square/count) << " stage=" << (stage3?3:stage2?2:1)
    << " pixel_type=" << (isFloat?"float32":"uint16")
    << " reference_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << '\n';
  if(different)throw std::runtime_error("Bridge samples differ from direct SDK reference");
}
