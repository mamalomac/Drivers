cmake_minimum_required(VERSION 3.16)
project(vkon11 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(VKON11_BUILD_TESTS "Build the self-test executables" ON)

# --- dependencies -----------------------------------------------------------
# Use local checkouts in third_party/ when present, otherwise download.
include(FetchContent)
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/SPIRV-Cross/CMakeLists.txt)
  set(SPIRV_CROSS_DIR ${CMAKE_SOURCE_DIR}/third_party/SPIRV-Cross)
else()
  FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
                       GIT_TAG vulkan-sdk-1.3.290.0 GIT_SHALLOW TRUE)
  FetchContent_GetProperties(spirv_cross)
  if(NOT spirv_cross_POPULATED)
    FetchContent_Populate(spirv_cross)
  endif()
  set(SPIRV_CROSS_DIR ${spirv_cross_SOURCE_DIR})
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include/vulkan/vulkan.h)
  set(VULKAN_HEADERS_DIR ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers)
else()
  FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                       GIT_TAG v1.3.290 GIT_SHALLOW TRUE)
  FetchContent_GetProperties(vulkan_headers)
  if(NOT vulkan_headers_POPULATED)
    FetchContent_Populate(vulkan_headers)
  endif()
  set(VULKAN_HEADERS_DIR ${vulkan_headers_SOURCE_DIR})
endif()

add_library(spirv-cross-hlsl-static STATIC
  ${SPIRV_CROSS_DIR}/spirv_cross.cpp
  ${SPIRV_CROSS_DIR}/spirv_parser.cpp
  ${SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp
  ${SPIRV_CROSS_DIR}/spirv_cfg.cpp
  ${SPIRV_CROSS_DIR}/spirv_glsl.cpp
  ${SPIRV_CROSS_DIR}/spirv_hlsl.cpp)
target_include_directories(spirv-cross-hlsl-static PUBLIC ${SPIRV_CROSS_DIR})

# --- the driver -------------------------------------------------------------
add_library(vkon11 SHARED
  src/icd.cpp src/instance.cpp src/device.cpp src/formats.cpp src/shader.cpp
  src/pipeline.cpp src/cmd.cpp src/swapchain.cpp src/vkon11.def)
set_target_properties(vkon11 PROPERTIES PREFIX "")
target_include_directories(vkon11 PRIVATE ${VULKAN_HEADERS_DIR}/include)
target_link_libraries(vkon11 PRIVATE spirv-cross-hlsl-static d3d11 dxgi dxguid)
if(MINGW)
  target_link_options(vkon11 PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--kill-at)
  target_compile_options(vkon11 PRIVATE -Wall -Wno-unknown-pragmas)
elseif(MSVC)
  target_compile_options(vkon11 PRIVATE /W3 /EHsc)
endif()

if(VKON11_BUILD_TESTS AND EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
