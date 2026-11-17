cmake_minimum_required(VERSION 3.20)
project(RawNodeMinimumResearch LANGUAGES C CXX)
if(NOT MSVC)
  message(FATAL_ERROR "Native MSVC research target")
endif()
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/MakeDng.py ${CMAKE_BINARY_DIR}/seed.dng COMMAND_ERROR_IS_FATAL ANY)
set(RAWNODE_ENABLE_JXL_DNG OFF CACHE BOOL "" FORCE)
include(${root}/cmake/RawDependencies.cmake)
find_package(TIFF REQUIRED)
find_path(lcms_include lcms2.h REQUIRED)
find_library(lcms_library NAMES lcms2 lcms REQUIRED)
set(imgui "${root}/native-deps/imgui")
add_library(ResearchCore STATIC
  ${root}/src/persist/WindowsFile.cpp
  ${root}/src/persist/ProjectPersistSave.cpp ${root}/src/persist/ProjectPersistLoad.cpp
  ${root}/src/persist/Json.cpp ${root}/src/persist/Preferences.cpp
  ${root}/src/imgio/ImageFormats.cpp
  ${root}/src/GraphModel.cpp ${root}/src/GraphValidation.cpp
  ${root}/src/color/ColorEncoding.cpp ${root}/src/color/TransferFunction.cpp ${root}/src/color/LinearColorTransform.cpp
  ${root}/src/ui/Themes.cpp ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp
  MinimalWriter.cpp)
target_include_directories(ResearchCore PUBLIC ${root}/src ${imgui} ${root}/native-deps/glfw/include)
target_compile_features(ResearchCore PUBLIC cxx_std_17)
target_compile_options(ResearchCore PUBLIC /utf-8 /Gy /Gw)
target_link_libraries(ResearchCore PUBLIC advapi32 bcrypt)
# Actual production image components, unchanged; the scoped variant changes only
# the documented stb UTF-8 compile option. No decoder stubs or source substitution.
foreach(variant IN ITEMS Default Scoped)
  add_library(Image${variant} STATIC ${root}/src/imgio/ImageLoad.cpp ${root}/src/imgio/ImageColor.cpp ${root}/src/imgio/ImageWrite.cpp ${root}/third_party/tinyexr/tinyexr.cc)
  target_include_directories(Image${variant} PRIVATE ${root}/third_party/stb ${root}/third_party/tinyexr ${lcms_include})
  target_link_libraries(Image${variant} PUBLIC ResearchCore RawNodeRaw TIFF::TIFF ${lcms_library} ZLIB::ZLIB)
  if(variant STREQUAL "Scoped")
    target_compile_definitions(Image${variant} PRIVATE STBI_WINDOWS_UTF8 STBIW_WINDOWS_UTF8)
  endif()
endforeach()
foreach(name IN ITEMS Probe ProbeUtf8 ProbeScoped)
  add_executable(${name} Probe.cpp)
  target_compile_definitions(${name} PRIVATE RESEARCH_DNG="${CMAKE_BINARY_DIR}/seed.dng")
  target_link_libraries(${name} PRIVATE ImageDefault)
  target_link_options(${name} PRIVATE /OPT:REF)
endforeach()
# Avoid linking both STB implementations in the scoped executable.
set_property(TARGET ProbeScoped PROPERTY LINK_LIBRARIES ImageScoped)
target_link_options(ProbeUtf8 PRIVATE "/MANIFESTINPUT:${CMAKE_CURRENT_LIST_DIR}/utf8.manifest")
