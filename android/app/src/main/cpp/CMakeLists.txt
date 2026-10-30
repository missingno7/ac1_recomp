cmake_minimum_required(VERSION 3.22.1)
project(ac1_android LANGUAGES C CXX)

get_filename_component(AC1_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(AC1_GENERATED_DIR "${AC1_ROOT}/work/lift_initial" CACHE PATH "Directory containing generated AC1 translation units")
set(AC1_DXVK_NATIVE_INCLUDE_DIR "${AC1_ROOT}/work/dxvk/prepared-3.1/include/native" CACHE PATH "Prepared DXVK 3.1 compatibility headers")
if(AC1_DXVK_NATIVE_INCLUDE_DIR STREQUAL "${AC1_ROOT}/work/dxvk/prepared/include/native")
    set(AC1_DXVK_NATIVE_INCLUDE_DIR "${AC1_ROOT}/work/dxvk/prepared-3.1/include/native" CACHE PATH "Prepared DXVK 3.1 compatibility headers" FORCE)
endif()
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(AC1_ABI_TAG arm64)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(AC1_ABI_TAG x86_64)
else()
    message(FATAL_ERROR "Unsupported Android ABI ${ANDROID_ABI}; use arm64-v8a or x86_64")
endif()
set(AC1_DXVK_ANDROID_LIBRARY "${AC1_ROOT}/work/build/dxvk-android-${AC1_ABI_TAG}/libac1_dxvk_android.so" CACHE FILEPATH "Prepared DXVK Native Android provider")
set(AC1_FFMPEG_PREFIX "${AC1_ROOT}/work/bink_native/ffmpeg-android-${AC1_ABI_TAG}/install" CACHE PATH "NDK static Bink FFmpeg prefix")
set(AC1_NATIVE_BINK ON CACHE BOOL "Use native Bink decoder on Android")
set(AC1_DIRECT_MEMORY ON CACHE BOOL "Reserve the 4 GiB guest arena")
set(AC1_FORCE_INLINE_ACCESSORS ON CACHE BOOL "Inline generated guest accesses")
if(ANDROID_PLATFORM_LEVEL LESS 29)
    message(FATAL_ERROR "AC1 Android requires API 29+ for native ELF TLS")
endif()
if(NOT EXISTS "${AC1_DXVK_NATIVE_INCLUDE_DIR}/directx/d3d9.h" OR
   NOT EXISTS "${AC1_DXVK_NATIVE_INCLUDE_DIR}/windows/windows_base.h")
    message(FATAL_ERROR "Prepared DXVK Native compatibility headers were not found. Run ports/dxvk_android/build.ps1 or set AC1_DXVK_NATIVE_INCLUDE_DIR.")
endif()
if(NOT EXISTS "${AC1_DXVK_ANDROID_LIBRARY}")
    message(FATAL_ERROR "Prepared ${ANDROID_ABI} DXVK Native shared library was not found. Run ports/dxvk_android/build.ps1 -Abi ${ANDROID_ABI} or set AC1_DXVK_ANDROID_LIBRARY.")
endif()
add_subdirectory("${AC1_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/ac1_core")
target_compile_definitions(ac1_runtime PRIVATE RECOMP_FS_SIMULATED AC1_HOST_D3D9=1)
target_sources(ac1_runtime PRIVATE
    "${AC1_ROOT}/src/graphics/ac1_d3d9_bridge.cpp"
    "${AC1_ROOT}/src/platform/ac1_audio_win32.c"
    "${AC1_ROOT}/src/platform/ac1_ds8_portable.cpp"
    "${AC1_ROOT}/src/platform/ac1_audio_aaudio.cpp"
    "${AC1_ROOT}/src/platform/ac1_dinput8_portable.cpp"
    "${AC1_ROOT}/src/platform/ac1_input_script.cpp"
    "${AC1_ROOT}/src/platform/ac1_window_android.c")
target_include_directories(ac1_runtime PRIVATE
    "${AC1_DXVK_NATIVE_INCLUDE_DIR}/windows"
    "${AC1_DXVK_NATIVE_INCLUDE_DIR}/directx")
target_compile_features(ac1_runtime PRIVATE cxx_std_17)
file(GLOB AC1_ANDROID_GENERATED CONFIGURE_DEPENDS
    "${AC1_GENERATED_DIR}/initial.c"
    "${AC1_GENERATED_DIR}/initial_*.c")
set_source_files_properties(${AC1_ANDROID_GENERATED} TARGET_DIRECTORY ac1_runtime PROPERTIES
    COMPILE_OPTIONS "-w;-fwrapv;-fno-strict-aliasing;-ffp-contract=off;$<$<STREQUAL:${AC1_GENERATED_DEBUG},lines>:-gline-tables-only>")

# Keep one implementation of the PE loader, guest memory, dispatch and generated
# functions. Only this small JNI adapter is Android-specific.
add_library(ac1_android SHARED native_bridge.c)
target_include_directories(ac1_android PRIVATE "${AC1_ROOT}/src/runtime" "${AC1_ROOT}/src/platform")
target_compile_features(ac1_android PRIVATE c_std_11)
target_compile_options(ac1_android PRIVATE -Wall -Wextra)
target_compile_definitions(ac1_android PRIVATE _GNU_SOURCE AC1_DXVK31=1)
target_include_directories(ac1_android PRIVATE "${AC1_ROOT}/work/dxvk/prepared-3.1/include/vulkan/include")
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_definitions(ac1_android PRIVATE AC1_HAS_TURNIP=1)
    target_include_directories(ac1_android PRIVATE "${AC1_ROOT}/work/deps/libadrenotools/include")
endif()
add_library(ac1_dxvk_android SHARED IMPORTED GLOBAL)
set_target_properties(ac1_dxvk_android PROPERTIES
    IMPORTED_LOCATION "${AC1_DXVK_ANDROID_LIBRARY}")
target_link_libraries(ac1_android PRIVATE ac1_runtime ac1_dxvk_android android log dl vulkan aaudio)
# Keep static-archive symbols local: FFmpeg's AArch64 NEON code addresses its
# tables PC-relatively, which lld rejects for preemptible exported symbols.
target_link_options(ac1_android PRIVATE -Wl,--exclude-libs,ALL)
