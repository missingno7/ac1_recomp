cmake_minimum_required(VERSION 3.22.1)
project(ac1_android LANGUAGES C CXX)

get_filename_component(AC1_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(AC1_GENERATED_DIR "${AC1_ROOT}/work/lift_initial" CACHE PATH "Directory containing generated AC1 translation units")
set(AC1_DXVK_NATIVE_INCLUDE_DIR "${AC1_ROOT}/work/dxvk/prepared/include/native" CACHE PATH "Prepared DXVK Native compatibility headers")
set(AC1_DXVK_ANDROID_LIBRARY "${AC1_ROOT}/work/build/dxvk-android-arm64/libac1_dxvk_android.so" CACHE FILEPATH "Prepared ARM64 DXVK Native Android provider")
if(NOT EXISTS "${AC1_DXVK_NATIVE_INCLUDE_DIR}/directx/d3d9.h" OR
   NOT EXISTS "${AC1_DXVK_NATIVE_INCLUDE_DIR}/windows/windows_base.h")
    message(FATAL_ERROR "Prepared DXVK Native compatibility headers were not found. Run ports/dxvk_android/build.ps1 or set AC1_DXVK_NATIVE_INCLUDE_DIR.")
endif()
if(NOT EXISTS "${AC1_DXVK_ANDROID_LIBRARY}")
    message(FATAL_ERROR "Prepared ARM64 DXVK Native shared library was not found. Run ports/dxvk_android/build.ps1 or set AC1_DXVK_ANDROID_LIBRARY.")
endif()
add_subdirectory("${AC1_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/ac1_core")
target_compile_definitions(ac1_runtime PRIVATE RECOMP_FS_SIMULATED AC1_HOST_D3D9=1)
target_sources(ac1_runtime PRIVATE "${AC1_ROOT}/src/graphics/ac1_d3d9_bridge.cpp")
target_include_directories(ac1_runtime PRIVATE
    "${AC1_DXVK_NATIVE_INCLUDE_DIR}/windows"
    "${AC1_DXVK_NATIVE_INCLUDE_DIR}/directx")
target_compile_features(ac1_runtime PRIVATE cxx_std_17)
file(GLOB AC1_ANDROID_GENERATED CONFIGURE_DEPENDS
    "${AC1_GENERATED_DIR}/initial.c"
    "${AC1_GENERATED_DIR}/initial_*.c")
set_source_files_properties(${AC1_ANDROID_GENERATED} TARGET_DIRECTORY ac1_runtime PROPERTIES
    COMPILE_OPTIONS "-O0;-w;-fwrapv;-fno-strict-aliasing;-ffp-contract=off")

# Keep one implementation of the PE loader, guest memory, dispatch and generated
# functions. Only this small JNI adapter is Android-specific.
add_library(ac1_android SHARED native_bridge.c)
target_include_directories(ac1_android PRIVATE "${AC1_ROOT}/src/runtime")
target_compile_features(ac1_android PRIVATE c_std_11)
target_compile_options(ac1_android PRIVATE -Wall -Wextra)
add_library(ac1_dxvk_android SHARED IMPORTED GLOBAL)
set_target_properties(ac1_dxvk_android PROPERTIES
    IMPORTED_LOCATION "${AC1_DXVK_ANDROID_LIBRARY}")
target_link_libraries(ac1_android PRIVATE ac1_runtime ac1_dxvk_android android log dl vulkan)
