cmake_minimum_required(VERSION 3.20)
project(RawNodePersistenceTests LANGUAGES C CXX)
if(NOT MSVC)
  message(FATAL_ERROR "This focused target validates native MSVC Windows persistence")
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
add_library(PersistenceCore STATIC
  ${root}/src/persist/WindowsFile.cpp
  ${root}/src/persist/ProjectPersistSave.cpp ${root}/src/persist/ProjectPersistLoad.cpp
  ${root}/src/persist/Json.cpp ${root}/src/persist/Preferences.cpp
  ${root}/src/imgio/ImageFormats.cpp
  ${root}/src/GraphModel.cpp ${root}/src/GraphValidation.cpp
  ${root}/src/color/ColorEncoding.cpp ${root}/src/color/TransferFunction.cpp ${root}/src/color/LinearColorTransform.cpp
  ${root}/src/ui/Themes.cpp
  ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp)
target_compile_features(PersistenceCore PUBLIC cxx_std_17)
target_include_directories(PersistenceCore PUBLIC ${root}/src ${root}/tests ${imgui} ${root}/native-deps/glfw/include)
target_compile_options(PersistenceCore PUBLIC /utf-8 /W4 /Gy /Gw)
target_link_libraries(PersistenceCore PUBLIC advapi32 bcrypt)
# Link the actual production decoders as well as persistence: narrow/wide path
# integration must be exercised through photograph discovery, not writer inputs.
add_library(PersistenceImages STATIC ${root}/src/imgio/ImageLoad.cpp ${root}/src/imgio/ImageColor.cpp
  ${root}/src/imgio/ImageWrite.cpp ${root}/third_party/tinyexr/tinyexr.cc)
target_include_directories(PersistenceImages PRIVATE ${root}/third_party/stb ${root}/third_party/tinyexr ${lcms_include})
target_link_libraries(PersistenceImages PUBLIC PersistenceCore RawNodeRaw TIFF::TIFF ${lcms_library} ZLIB::ZLIB)
foreach(name IN ITEMS WindowsPersistenceTests WindowsCodePageTests)
  add_executable(${name} ../WindowsPersistenceTests.cpp ../SidecarFileTests.cpp)
  target_compile_definitions(${name} PRIVATE RAWNODE_TEST_DNG="${CMAKE_BINARY_DIR}/seed.dng")
  target_link_libraries(${name} PRIVATE PersistenceImages)
  target_link_options(${name} PRIVATE /OPT:REF)
endforeach()
target_link_options(WindowsPersistenceTests PRIVATE "/MANIFESTINPUT:${root}/src/platform/WindowsUtf8.manifest")
