cmake_minimum_required(VERSION 3.22)
project(AC1DXVKAndroid C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "AC1 DXVK target requires Android arm64-v8a")
endif()

set(DXVK_SOURCE "" CACHE PATH "Prepared DXVK Native source directory")
if(NOT EXISTS "${DXVK_SOURCE}/PINNED_REVISION")
  message(FATAL_ERROR "Run ports/dxvk_android/build.ps1 to fetch and prepare the pinned DXVK source")
endif()
file(READ "${DXVK_SOURCE}/PINNED_REVISION" DXVK_REVISION)
string(STRIP "${DXVK_REVISION}" DXVK_REVISION)
if(NOT DXVK_REVISION STREQUAL "c8dc91fabd00cac11d697ccf07426e798393cd40")
  message(FATAL_ERROR "Unexpected DXVK Native revision: ${DXVK_REVISION}")
endif()
if(NOT EXISTS "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256")
  message(FATAL_ERROR "DXVK Android preparation marker is missing; rerun build.ps1")
endif()
file(READ "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256" DXVK_PREPARED_HASH)
string(STRIP "${DXVK_PREPARED_HASH}" DXVK_PREPARED_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/prepare.py" DXVK_EXPECTED_HASH)
if(NOT DXVK_PREPARED_HASH STREQUAL DXVK_EXPECTED_HASH)
  message(FATAL_ERROR "DXVK Android patches are stale; rerun build.ps1")
endif()
if(NOT EXISTS "${DXVK_SOURCE}/generated/dxvk_blit_frag_1d.h")
  message(FATAL_ERROR "DXVK built-in shaders are missing; run shaders.py through build.ps1")
endif()

file(GLOB UTIL
  "${DXVK_SOURCE}/src/util/*.cpp" "${DXVK_SOURCE}/src/util/com/*.cpp"
  "${DXVK_SOURCE}/src/util/config/*.cpp" "${DXVK_SOURCE}/src/util/log/*.cpp"
  "${DXVK_SOURCE}/src/util/sha1/*.c" "${DXVK_SOURCE}/src/util/sha1/*.cpp"
  "${DXVK_SOURCE}/src/util/sync/*.cpp")
list(FILTER UTIL EXCLUDE REGEX "util_gdi.cpp$")
list(APPEND UTIL
  "${DXVK_SOURCE}/src/util/platform/util_env_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/util_luid_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/util_string_linux.cpp"
  "${DXVK_SOURCE}/src/util/platform/thread_native.cpp")
file(GLOB GRAPHICS
  "${DXVK_SOURCE}/src/dxso/*.cpp" "${DXVK_SOURCE}/src/spirv/*.cpp"
  "${DXVK_SOURCE}/src/vulkan/*.cpp" "${DXVK_SOURCE}/src/dxvk/*.cpp"
  "${DXVK_SOURCE}/src/dxvk/hud/*.cpp" "${DXVK_SOURCE}/src/d3d9/*.cpp")
list(FILTER GRAPHICS EXCLUDE REGEX "dxvk_open(vr|xr).cpp$")

add_library(ac1_dxvk_android SHARED ${UTIL} ${GRAPHICS} android_wsi.cpp
  "${DXVK_SOURCE}/src/dxvk/dxvk_pipecache.cpp")
target_compile_features(ac1_dxvk_android PRIVATE cxx_std_17)
target_compile_definitions(ac1_dxvk_android PRIVATE
  DXVK_NATIVE DXVK_PLATFORM_LINUX DXVK_WSI_ANDROID VK_USE_PLATFORM_ANDROID_KHR NOMINMAX)
target_include_directories(ac1_dxvk_android PRIVATE
  "${DXVK_SOURCE}" "${DXVK_SOURCE}/include" "${DXVK_SOURCE}/src/util"
  "${DXVK_SOURCE}/include/native" "${DXVK_SOURCE}/include/native/windows"
  "${DXVK_SOURCE}/include/native/directx" "${DXVK_SOURCE}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ac1_dxvk_android PRIVATE -O2 -Wno-deprecated-declarations)
find_library(ANDROID_VULKAN_LIBRARY vulkan REQUIRED)
target_link_libraries(ac1_dxvk_android PRIVATE ${ANDROID_VULKAN_LIBRARY} android log dl)
