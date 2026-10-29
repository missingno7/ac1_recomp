cmake_minimum_required(VERSION 3.22)
project(ac1_recomp C)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(AC1_TRACE_EIP "Record the last original instruction address for startup diagnostics" ON)
option(AC1_HOST_D3D9 "Build the native Windows D3D9 bridge" ${WIN32})
set(AC1_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/lift_initial" CACHE PATH "Private generated AC1 C source")
set(AC1_BINK_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/lift_bink" CACHE PATH "Private generated Bink C source")
file(GLOB AC1_GENERATED CONFIGURE_DEPENDS
  "${AC1_GENERATED_DIR}/initial.c"
  "${AC1_GENERATED_DIR}/initial_*.c")
file(GLOB AC1_BINK_GENERATED CONFIGURE_DEPENDS
  "${AC1_BINK_GENERATED_DIR}/initial_*.c"
  "${AC1_BINK_GENERATED_DIR}/bink_dispatch.c")
if(NOT AC1_GENERATED)
  message(FATAL_ERROR "Generate real AC1 functions with tools/lift_ac1.py before building")
endif()
if(NOT AC1_BINK_GENERATED)
  message(FATAL_ERROR "Generate the pinned Bink module with tools/lift_bink.py before building")
endif()

add_library(ac1_runtime STATIC
  src/runtime/ac1_runtime.c
  src/runtime/ac1_network.c
  src/runtime/ac1_files.c
  src/runtime/ac1_crypto.c
  src/runtime/guest_clock.c
  src/runtime/guest_memory.c
  src/runtime/pe32_loader.c
  src/runtime/pcrecomp_heap.c
  ${AC1_GENERATED}
  ${AC1_BINK_GENERATED})
set_target_properties(ac1_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ac1_runtime PUBLIC Threads::Threads)
if(WIN32)
  enable_language(CXX)
  target_sources(ac1_runtime PRIVATE
    src/platform/ac1_window_win32.c
    src/platform/ac1_audio_win32.c
    src/platform/ac1_ds8_bridge.cpp
    src/platform/ac1_dinput8_bridge.cpp)
  target_compile_features(ac1_runtime PRIVATE cxx_std_17)
  target_include_directories(ac1_runtime PRIVATE src/platform)
  target_link_libraries(ac1_runtime PRIVATE ws2_32 user32 gdi32 advapi32)
endif()
if(AC1_HOST_D3D9)
  if(NOT WIN32)
    message(FATAL_ERROR "AC1_HOST_D3D9 currently requires Windows")
  endif()
  set(AC1_D3D9_BUILD_SMOKE OFF)
  add_subdirectory(src/graphics)
  target_compile_definitions(ac1_runtime PRIVATE AC1_HOST_D3D9=1)
  target_link_libraries(ac1_runtime PRIVATE ac1_d3d9_bridge)
endif()
target_include_directories(ac1_runtime PUBLIC src/runtime "${AC1_GENERATED_DIR}")
set_source_files_properties(src/runtime/ac1_runtime.c PROPERTIES
  INCLUDE_DIRECTORIES "${AC1_BINK_GENERATED_DIR}")
if(AC1_TRACE_EIP)
  target_compile_definitions(ac1_runtime PRIVATE AC1_TRACE_EIP)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ac1_runtime PRIVATE -fwrapv -fno-strict-aliasing -ffp-contract=off)
  set_source_files_properties(src/runtime/ac1_runtime.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror=return-type;-Werror=implicit-function-declaration")
endif()
if(NOT WIN32)
  target_link_libraries(ac1_runtime PUBLIC m)
endif()
if(NOT ANDROID)
  target_compile_definitions(ac1_runtime PRIVATE AC1_DEFAULT_USERDATA_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/work/userdata")
  add_executable(ac1_startup src/platform/main.c)
  target_link_libraries(ac1_startup PRIVATE ac1_runtime)
  add_executable(ac1_repeat_run EXCLUDE_FROM_ALL tests/repeat_run.c)
  target_link_libraries(ac1_repeat_run PRIVATE ac1_runtime)
  add_executable(ac1_network_contract EXCLUDE_FROM_ALL tests/network_contract.c)
  target_link_libraries(ac1_network_contract PRIVATE ac1_runtime)
  add_executable(ac1_crypto_contract EXCLUDE_FROM_ALL
    tests/crypto_contract.c src/runtime/ac1_crypto.c)
  target_include_directories(ac1_crypto_contract PRIVATE src/runtime)
  target_link_libraries(ac1_crypto_contract PRIVATE Threads::Threads)
  add_executable(ac1_files_contract EXCLUDE_FROM_ALL
    tests/files_contract.c src/runtime/ac1_files.c)
  target_include_directories(ac1_files_contract PRIVATE src/runtime)
  add_executable(ac1_guest_memory_contract EXCLUDE_FROM_ALL
    tests/guest_memory_scalar_fixture.c src/runtime/guest_memory.c)
  target_include_directories(ac1_guest_memory_contract PRIVATE src/runtime)
  add_executable(ac1_profile_int_contract EXCLUDE_FROM_ALL
    tests/ac1_profile_int_fixture.c)
  target_include_directories(ac1_profile_int_contract PRIVATE src/runtime)
  add_executable(ac1_lookup_cache_contract EXCLUDE_FROM_ALL
    tests/lifted_lookup_cache_fixture.c)
  target_include_directories(ac1_lookup_cache_contract PRIVATE src/runtime)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ac1_guest_memory_contract PRIVATE
      -fwrapv -fno-strict-aliasing -ffp-contract=off)
  endif()
  if(WIN32)
    target_link_libraries(ac1_crypto_contract PRIVATE advapi32)
  endif()
  if(WIN32)
    add_executable(ac1_window_smoke EXCLUDE_FROM_ALL tests/ac1_window_win32_smoke.c)
    target_include_directories(ac1_window_smoke PRIVATE src/platform)
    target_link_libraries(ac1_window_smoke PRIVATE ac1_runtime)
    add_executable(ac1_dinput8_smoke EXCLUDE_FROM_ALL src/platform/tests/dinput8_bridge_smoke.cpp)
    target_compile_features(ac1_dinput8_smoke PRIVATE cxx_std_17)
    target_link_libraries(ac1_dinput8_smoke PRIVATE ac1_runtime)
  endif()
endif()
