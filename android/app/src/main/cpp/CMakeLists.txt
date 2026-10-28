cmake_minimum_required(VERSION 3.22.1)
project(ac1_android C)

get_filename_component(AC1_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(AC1_GENERATED_DIR "${AC1_ROOT}/work/lift_initial" CACHE PATH "Directory containing generated AC1 translation units")
add_subdirectory("${AC1_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/ac1_core")
target_compile_definitions(ac1_runtime PRIVATE RECOMP_FS_SIMULATED)
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
target_link_libraries(ac1_android PRIVATE ac1_runtime android log dl)
