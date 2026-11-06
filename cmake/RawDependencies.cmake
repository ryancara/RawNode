# One pinned LibRaw implementation for every configuration. The option adds
# codecs to that same library; it never selects a historical/system RAW backend.
option(RAWNODE_ENABLE_JXL_DNG "Enable integer JPEG XL DNG decoding with Adobe SDK 2768" OFF)

FetchContent_Declare(rawnode_libraw
  URL https://codeload.github.com/LibRaw/LibRaw/tar.gz/refs/tags/0.22.3
  URL_HASH SHA256=7edf96c5b70cff88980a02ab0fc8af125cb6a4a377985573c654439d9d02355c
)
FetchContent_MakeAvailable(rawnode_libraw)

find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenMP QUIET COMPONENTS CXX)

file(GLOB_RECURSE rawnode_raw_sources CONFIGURE_DEPENDS "${rawnode_libraw_SOURCE_DIR}/src/*.cpp")
list(FILTER rawnode_raw_sources EXCLUDE REGEX "_ph\\.cpp$")
add_library(RawNodeRaw STATIC ${rawnode_raw_sources})
target_include_directories(RawNodeRaw PUBLIC "${rawnode_libraw_SOURCE_DIR}")
target_compile_definitions(RawNodeRaw PRIVATE USE_JPEG USE_ZLIB)
target_compile_definitions(RawNodeRaw PUBLIC LIBRAW_NODLL RAWNODE_ENABLE_JXL_DNG=$<BOOL:${RAWNODE_ENABLE_JXL_DNG}>)
target_link_libraries(RawNodeRaw PUBLIC JPEG::JPEG ZLIB::ZLIB Threads::Threads)
if(OpenMP_CXX_FOUND)
  target_link_libraries(RawNodeRaw PUBLIC OpenMP::OpenMP_CXX)
else()
  message(STATUS "OpenMP unavailable: LibRaw 0.22.3 will use serial processing")
endif()

if(RAWNODE_ENABLE_JXL_DNG)
  find_package(Python3 3 REQUIRED COMPONENTS Interpreter)
  # Populate upstream sources before configuring libjxl: its required submodules
  # live at these exact gitlinks. No testdata, tools or unrelated submodules.
  macro(rawnode_populate dependency)
    FetchContent_GetProperties(${dependency})
    if(NOT ${dependency}_POPULATED)
      FetchContent_Populate(${dependency})
    endif()
  endmacro()
  cmake_policy(SET CMP0097 NEW)
  FetchContent_Declare(rawnode_jxl
    GIT_REPOSITORY https://github.com/libjxl/libjxl.git
    GIT_TAG 332feb17d17311c748445f7ee75c4fb55cc38530
    GIT_CONFIG core.autocrlf=false core.eol=lf
    GIT_SUBMODULES ""
  )
  rawnode_populate(rawnode_jxl)
  FetchContent_Declare(rawnode_highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG 457c891775a7397bdb0376bb1031e6e027af1c48
    GIT_SUBMODULES ""
    SOURCE_DIR "${rawnode_jxl_SOURCE_DIR}/third_party/highway"
  )
  FetchContent_Declare(rawnode_brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG 36533a866ed1ca4b75cf049f4521e4ec5fe24727
    GIT_SUBMODULES ""
    SOURCE_DIR "${rawnode_jxl_SOURCE_DIR}/third_party/brotli"
  )
  FetchContent_Declare(rawnode_skcms
    GIT_REPOSITORY https://github.com/google/skcms.git
    GIT_TAG b2e692629c1fb19342517d7fb61f1cf83d075492
    GIT_SUBMODULES ""
    SOURCE_DIR "${rawnode_jxl_SOURCE_DIR}/third_party/skcms"
  )
  rawnode_populate(rawnode_highway)
  rawnode_populate(rawnode_brotli)
  rawnode_populate(rawnode_skcms)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patches/prepare_libjxl.py"
    "${rawnode_jxl_SOURCE_DIR}" RESULT_VARIABLE rawnode_jxl_patch)
  if(NOT rawnode_jxl_patch EQUAL 0)
    message(FATAL_ERROR "Could not verify/apply the pinned libjxl encoder initialization fix")
  endif()
  # Scope generic dependency options to libjxl; do not overwrite the host's cache.
  function(rawnode_add_jxl)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    foreach(feature TOOLS DEVTOOLS JPEGLI JPEGLI_LIBJPEG DOXYGEN MANPAGES BENCHMARK
        EXAMPLES JNI SJPEG OPENEXR VIEWERS TCMALLOC PLUGINS)
      set(JPEGXL_ENABLE_${feature} OFF)
    endforeach()
    set(JPEGXL_ENABLE_SKCMS ON)
    add_subdirectory("${rawnode_jxl_SOURCE_DIR}" "${rawnode_jxl_BINARY_DIR}" EXCLUDE_FROM_ALL)
  endfunction()
  rawnode_add_jxl()

  set(rawnode_sdk_source "${PROJECT_SOURCE_DIR}/third_party/dng_sdk/source")
  file(GLOB rawnode_sdk_sources CONFIGURE_DEPENDS "${rawnode_sdk_source}/*.cpp")
  # Included directly by dng_lossless_jpeg.cpp; do not compile it twice.
  list(FILTER rawnode_sdk_sources EXCLUDE REGEX "/dng_lossless_jpeg_shared\\.cpp$")
  list(REMOVE_ITEM rawnode_sdk_sources "${rawnode_sdk_source}/dng_jxl.cpp")
  set(rawnode_jxl_compat "${CMAKE_CURRENT_BINARY_DIR}/dng_jxl_no_xmp.cpp")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patches/prepare_dng_sdk.py"
    "${rawnode_sdk_source}/dng_jxl.cpp" "${rawnode_jxl_compat}" RESULT_VARIABLE rawnode_sdk_patch)
  if(NOT rawnode_sdk_patch EQUAL 0)
    message(FATAL_ERROR "Could not prepare SDK 2768's XMP-disabled compatibility copy")
  endif()
  list(APPEND rawnode_sdk_sources "${rawnode_jxl_compat}")
  add_library(RawNodeDngSdk STATIC ${rawnode_sdk_sources})
  target_include_directories(RawNodeDngSdk PUBLIC "${rawnode_sdk_source}")
  if(APPLE)
    set(rawnode_sdk_platform qMacOS=1)
    find_library(rawnode_corefoundation CoreFoundation)
    find_library(rawnode_coreservices CoreServices)
    if(NOT rawnode_corefoundation OR NOT rawnode_coreservices)
      message(FATAL_ERROR "The Adobe SDK requires CoreFoundation and CoreServices")
    endif()
    target_link_libraries(RawNodeDngSdk PUBLIC ${rawnode_corefoundation} ${rawnode_coreservices})
  elseif(WIN32)
    set(rawnode_sdk_platform qWinOS=1)
    target_link_libraries(RawNodeDngSdk PUBLIC user32)
  else()
    set(rawnode_sdk_platform qLinux=1)
  endif()
  target_compile_definitions(RawNodeDngSdk PUBLIC ${rawnode_sdk_platform} qDNGThreadSafe=1
    qDNGUseXMP=0 qDNGXMPFiles=0 qDNGXMPDocOps=0 qDNGUseLibJPEG=1 qDNGValidateTarget=0)
  target_link_libraries(RawNodeDngSdk PUBLIC jxl jxl_threads JPEG::JPEG ZLIB::ZLIB Threads::Threads)
  target_compile_definitions(RawNodeRaw PRIVATE USE_DNGSDK)
  target_link_libraries(RawNodeRaw PUBLIC RawNodeDngSdk)
endif()

# Keep fetched-library notices alongside build outputs for packaging. The pinned
# source locations and binary redistribution obligations are documented separately.
set(rawnode_notices "${CMAKE_BINARY_DIR}/RawDependencyNotices")
file(MAKE_DIRECTORY "${rawnode_notices}/LibRaw")
file(COPY "${rawnode_libraw_SOURCE_DIR}/COPYRIGHT"
  "${rawnode_libraw_SOURCE_DIR}/LICENSE.CDDL" "${rawnode_libraw_SOURCE_DIR}/LICENSE.LGPL"
  DESTINATION "${rawnode_notices}/LibRaw")
if(RAWNODE_ENABLE_JXL_DNG)
  file(MAKE_DIRECTORY "${rawnode_notices}/Adobe" "${rawnode_notices}/libjxl"
    "${rawnode_notices}/Highway" "${rawnode_notices}/Brotli" "${rawnode_notices}/skcms")
  file(COPY "${PROJECT_SOURCE_DIR}/third_party/dng_sdk/LICENSE.txt"
    "${PROJECT_SOURCE_DIR}/third_party/dng_sdk/THIRD_PARTY_NOTICES.md"
    "${PROJECT_SOURCE_DIR}/third_party/dng_sdk/licenses" DESTINATION "${rawnode_notices}/Adobe")
  file(COPY "${rawnode_jxl_SOURCE_DIR}/LICENSE" "${rawnode_jxl_SOURCE_DIR}/PATENTS"
    "${rawnode_jxl_SOURCE_DIR}/AUTHORS" DESTINATION "${rawnode_notices}/libjxl")
  file(COPY "${rawnode_highway_SOURCE_DIR}/LICENSE" "${rawnode_highway_SOURCE_DIR}/LICENSE-BSD3"
    DESTINATION "${rawnode_notices}/Highway")
  file(COPY "${rawnode_brotli_SOURCE_DIR}/LICENSE" DESTINATION "${rawnode_notices}/Brotli")
  file(COPY "${rawnode_skcms_SOURCE_DIR}/LICENSE" DESTINATION "${rawnode_notices}/skcms")
endif()
