cmake_minimum_required(VERSION 3.22)
project(AC1DXVKAndroid C CXX)
if(NOT ANDROID OR NOT ANDROID_ABI MATCHES "^(arm64-v8a|x86_64)$")
  message(FATAL_ERROR "DXVK Android requires an NDK arm64-v8a or x86_64 toolchain")
endif()
set(DXVK_SOURCE "" CACHE PATH "Prepared official DXVK 3.1 sources")
file(READ "${DXVK_SOURCE}/PINNED_REVISION" DXVK_REVISION)
string(STRIP "${DXVK_REVISION}" DXVK_REVISION)
if(NOT DXVK_REVISION STREQUAL "70d7508c01201ed3d4bfb33da42ba834eafe3857")
  message(FATAL_ERROR "Unexpected upstream DXVK revision")
endif()
file(READ "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256" DXVK_PREPARED_HASH)
string(STRIP "${DXVK_PREPARED_HASH}" DXVK_PREPARED_HASH)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/prepare.py" DXVK_EXPECTED_HASH)
if(NOT DXVK_PREPARED_HASH STREQUAL DXVK_EXPECTED_HASH)
  message(FATAL_ERROR "Android preparation is stale; rerun build.ps1")
endif()
file(GLOB UTIL "${DXVK_SOURCE}/src/util/*.cpp" "${DXVK_SOURCE}/src/util/com/*.cpp"
  "${DXVK_SOURCE}/src/util/config/*.cpp" "${DXVK_SOURCE}/src/util/log/*.cpp"
  "${DXVK_SOURCE}/src/util/sha1/*.c" "${DXVK_SOURCE}/src/util/sha1/*.cpp"
  "${DXVK_SOURCE}/src/util/sync/*.cpp")
file(GLOB GRAPHICS "${DXVK_SOURCE}/src/spirv/*.cpp" "${DXVK_SOURCE}/src/vulkan/*.cpp"
  "${DXVK_SOURCE}/src/dxvk/*.cpp" "${DXVK_SOURCE}/src/dxvk/hud/*.cpp"
  "${DXVK_SOURCE}/src/d3d9/*.cpp" "${DXVK_SOURCE}/src/wsi/*.cpp")
list(FILTER GRAPHICS EXCLUDE REGEX "dxvk_open(vr|xr).cpp$")
set(DXBC "${DXVK_SOURCE}/subprojects/dxbc-spirv")
file(GLOB DXBC_SOURCES "${DXBC}/dxbc/*.cpp" "${DXBC}/ir/*.cpp" "${DXBC}/ir/passes/*.cpp"
  "${DXBC}/sm3/*.cpp" "${DXBC}/spirv/*.cpp" "${DXBC}/util/*.cpp")
set(DISPLAY "${DXVK_SOURCE}/subprojects/libdisplay-info")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pnp-id-table.c"
  COMMAND "${Python3_EXECUTABLE}" "${DISPLAY}/tool/gen-search-table.py" "${DISPLAY}/pnp.ids"
    "${CMAKE_CURRENT_BINARY_DIR}/pnp-id-table.c" pnp_id_table
  DEPENDS "${DISPLAY}/tool/gen-search-table.py" "${DISPLAY}/pnp.ids")
add_library(ac1_dxvk_android SHARED ${UTIL} ${GRAPHICS} ${DXBC_SOURCES}
  "${DISPLAY}/cta.c" "${DISPLAY}/displayid.c" "${DISPLAY}/dmt-table.c"
  "${DISPLAY}/edid.c" "${DISPLAY}/gtf.c" "${DISPLAY}/info.c"
  "${DISPLAY}/log.c" "${DISPLAY}/memory-stream.c" "${CMAKE_CURRENT_BINARY_DIR}/pnp-id-table.c" android_wsi.cpp)
target_compile_features(ac1_dxvk_android PRIVATE cxx_std_17)
target_compile_definitions(ac1_dxvk_android PRIVATE DXVK_WSI_ANDROID VK_USE_PLATFORM_ANDROID_KHR NOMINMAX _GNU_SOURCE
  DXBC_SPV_ENABLE_SM3 DXBC_SPV_ENABLE_SM5 DXBC_SPV_ENABLE_SPIRV)
set_source_files_properties("${DISPLAY}/cta.c" "${DISPLAY}/displayid.c" "${DISPLAY}/dmt-table.c"
  "${DISPLAY}/edid.c" "${DISPLAY}/gtf.c" "${DISPLAY}/info.c"
  "${DISPLAY}/log.c" "${DISPLAY}/memory-stream.c" PROPERTIES COMPILE_DEFINITIONS "static_array=static")
target_include_directories(ac1_dxvk_android PRIVATE "${DXVK_SOURCE}" "${DXVK_SOURCE}/include" "${DXVK_SOURCE}/src/util"
  "${DXVK_SOURCE}/include/vulkan/include" "${DXVK_SOURCE}/include/spirv/include"
  "${DXVK_SOURCE}/include/native" "${DXVK_SOURCE}/include/native/windows"
  "${DXVK_SOURCE}/include/native/directx" "${DXVK_SOURCE}/generated"
  "${DXBC}" "${DISPLAY}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ac1_dxvk_android PRIVATE -O2 -Wno-deprecated-declarations -Wno-missing-field-initializers)
target_link_libraries(ac1_dxvk_android PRIVATE android log dl)
