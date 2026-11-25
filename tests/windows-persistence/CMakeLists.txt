cmake_minimum_required(VERSION 3.20)
project(RawNodePersistenceTests LANGUAGES C CXX)
if(NOT MSVC)
  message(FATAL_ERROR "This target validates native MSVC Windows persistence")
endif()

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/MakeDng.py
  ${CMAKE_CURRENT_BINARY_DIR}/seed.dng COMMAND_ERROR_IS_FATAL ANY)
include(${root}/cmake/RawDependencies.cmake)
find_package(TIFF REQUIRED)
find_path(lcms_include lcms2.h REQUIRED)
find_library(lcms_library NAMES lcms2 lcms REQUIRED)
set(imgui "${root}/native-deps/imgui")

# Build the production sources without the GUI and render runtime.
add_library(PersistenceCore STATIC
  ${root}/src/persist/ProjectPersistSave.cpp ${root}/src/persist/ProjectPersistLoad.cpp
  ${root}/src/persist/Json.cpp ${root}/src/persist/Preferences.cpp
  ${root}/src/platform/DirectoryDiscovery.cpp
  ${root}/src/GraphModel.cpp ${root}/src/GraphValidation.cpp
  ${root}/src/color/ColorEncoding.cpp ${root}/src/color/TransferFunction.cpp
  ${root}/src/color/LinearColorTransform.cpp ${root}/src/ui/Themes.cpp
  ${root}/src/ui/KeyboardBindings.cpp
  ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp)
target_compile_features(PersistenceCore PUBLIC cxx_std_17)
target_include_directories(PersistenceCore PUBLIC ${root}/src ${root}/tests ${imgui} ${root}/native-deps/glfw/include)
target_compile_options(PersistenceCore PUBLIC /utf-8 /Gy /Gw)

add_library(PersistenceImages STATIC ${root}/src/imgio/ImageLoad.cpp ${root}/src/imgio/ImageColor.cpp
  ${root}/src/imgio/ImageWrite.cpp ${root}/third_party/tinyexr/tinyexr.cc)
target_include_directories(PersistenceImages PRIVATE ${root}/third_party/stb ${root}/third_party/tinyexr ${lcms_include})
target_link_libraries(PersistenceImages PUBLIC PersistenceCore RawNodeRaw TIFF::TIFF ${lcms_library} ZLIB::ZLIB)

add_executable(WindowsPersistenceTests Main.cpp ../SidecarFileTests.cpp)
target_compile_definitions(WindowsPersistenceTests PRIVATE RAWNODE_TEST_DNG="${CMAKE_CURRENT_BINARY_DIR}/seed.dng")
target_link_libraries(WindowsPersistenceTests PRIVATE PersistenceImages advapi32)
target_link_options(WindowsPersistenceTests PRIVATE /OPT:REF "/MANIFESTINPUT:${root}/src/platform/WindowsUtf8.manifest")
