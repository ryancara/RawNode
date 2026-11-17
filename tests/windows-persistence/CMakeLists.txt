cmake_minimum_required(VERSION 3.16)
project(RawNodePersistenceTests LANGUAGES CXX)
if(NOT MSVC)
  message(FATAL_ERROR "This focused target validates native MSVC Windows persistence")
endif()
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(imgui "${root}/native-deps/imgui")
add_executable(WindowsPersistenceTests
  ../WindowsPersistenceTests.cpp ../SidecarFileTests.cpp
  ${root}/src/persist/WindowsFile.cpp
  ${root}/src/persist/ProjectPersistSave.cpp ${root}/src/persist/ProjectPersistLoad.cpp
  ${root}/src/persist/Json.cpp ${root}/src/persist/Preferences.cpp
  ${root}/src/GraphModel.cpp ${root}/src/GraphValidation.cpp
  ${root}/src/color/ColorEncoding.cpp ${root}/src/color/TransferFunction.cpp
  ${root}/src/ui/Themes.cpp
  ${imgui}/imgui.cpp ${imgui}/imgui_draw.cpp ${imgui}/imgui_tables.cpp ${imgui}/imgui_widgets.cpp)
target_compile_features(WindowsPersistenceTests PRIVATE cxx_std_17)
target_include_directories(WindowsPersistenceTests PRIVATE ${root}/src ${root}/tests ${imgui} ${root}/native-deps/glfw/include)
# Normal function-level linking omits unused renderer/image-decoder entry points;
# the exact production codecs, file writer, preferences and validators are linked.
target_compile_options(WindowsPersistenceTests PRIVATE /utf-8 /W4 /Gy /Gw)
target_link_options(WindowsPersistenceTests PRIVATE /OPT:REF)
target_link_libraries(WindowsPersistenceTests PRIVATE advapi32 bcrypt)
