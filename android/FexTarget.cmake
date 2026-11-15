# Included by the pinned FEX tree after its embedding libraries have been configured.
get_filename_component(TEMPEST_PORT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(TEMPEST_BRIDGE "${TEMPEST_PORT}/third_party/fex_bridge")
add_library(tempest-fex-bridge STATIC
    "${TEMPEST_BRIDGE}/core/fex/fex_guest_engine.cpp"
    "${TEMPEST_BRIDGE}/core/guest_cpu/fex_guest_cpu.cpp"
    "${TEMPEST_BRIDGE}/core/guest_cpu/fex_hle_bridge.cpp"
    "${TEMPEST_BRIDGE}/core/guest_cpu/hle_call_adapter.cpp")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(TEMPEST_HOST_CLANG NAMES clang REQUIRED)
set(TEMPEST_AST_COMPILER "${TEMPEST_HOST_CLANG}")
set(TEMPEST_AST_TARGET aarch64-linux-gnu)
set(TEMPEST_AST_FLAGS)
if (ANDROID)
    set(TEMPEST_AST_COMPILER "${CMAKE_C_COMPILER}")
    set(TEMPEST_AST_TARGET "${CMAKE_C_COMPILER_TARGET}")
    list(APPEND TEMPEST_AST_FLAGS "--sysroot=${CMAKE_SYSROOT}")
    if(TEMPEST_ANDROID_SDL)
        list(APPEND TEMPEST_AST_FLAGS "-I${TEMPEST_ANDROID_SDL}/include")
    endif()
endif()
function(tempest_c_adapter target source)
get_filename_component(stem "${source}" NAME_WE)
set(adapter "${CMAKE_CURRENT_BINARY_DIR}/tempest/${stem}_fex.c")
if (NOT TEMPEST_GENERATED_${stem})
add_custom_command(OUTPUT "${adapter}"
    COMMAND "${Python3_EXECUTABLE}" "${TEMPEST_PORT}/tools/fex_exports.py"
        "${source}" --out "${adapter}"
        --compiler "${TEMPEST_AST_COMPILER}" --target "${TEMPEST_AST_TARGET}"
        ${TEMPEST_AST_FLAGS} -DTEMPEST_FEX "-I${TEMPEST_PORT}" "-I${TEMPEST_PORT}/src" "-I${TEMPEST_PORT}/third_party/libatrac9"
    DEPENDS "${TEMPEST_PORT}/tools/fex_exports.py" "${source}"
        "${TEMPEST_PORT}/src/runtime.h" "${TEMPEST_PORT}/src/guest_call.h" VERBATIM COMMAND_EXPAND_LISTS)
set(TEMPEST_GENERATED_${stem} TRUE PARENT_SCOPE)
set_source_files_properties("${adapter}" PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror" C_STANDARD 11)
endif()
target_sources(${target} PRIVATE "${adapter}")
endfunction()
add_executable(tempest-fex-check "${TEMPEST_PORT}/tests/test_fex_runtime.cpp")
tempest_c_adapter(tempest-fex-check "${TEMPEST_PORT}/src/runtime_ngs2.c")
tempest_c_adapter(tempest-fex-check "${TEMPEST_PORT}/tests/fex_abi_fixture.c")
file(GLOB TEMPEST_ATRAC9 CONFIGURE_DEPENDS "${TEMPEST_PORT}/third_party/libatrac9/*.c")
add_library(tempest-atrac9 STATIC ${TEMPEST_ATRAC9})
set_source_files_properties(${TEMPEST_ATRAC9} PROPERTIES COMPILE_OPTIONS "-w" C_STANDARD 99)
target_compile_features(tempest-fex-bridge PUBLIC cxx_std_20)
target_include_directories(tempest-fex-bridge PUBLIC "${TEMPEST_PORT}" "${TEMPEST_PORT}/src"
    "${TEMPEST_PORT}/android"
    "${TEMPEST_PORT}/third_party/libatrac9" "${TEMPEST_BRIDGE}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_SOURCE_DIR}/Source/Tools/CommonTools")
find_package(Threads REQUIRED)
target_link_libraries(tempest-fex-bridge PUBLIC FEXCore Common CommonTools JemallocLibs Threads::Threads m)
target_compile_definitions(tempest-fex-check PRIVATE _DEFAULT_SOURCE)
target_link_libraries(tempest-fex-check PRIVATE tempest-fex-bridge tempest-atrac9)

# Headless execution of the user's prepared image. SDL/GPU imports remain unsupported.
add_executable(tempest-boot-check "${TEMPEST_PORT}/android/boot_check.cpp"
    "${TEMPEST_PORT}/android/fex_runtime.cpp")
file(GLOB TEMPEST_RUNTIME CONFIGURE_DEPENDS "${TEMPEST_PORT}/src/runtime*.c")
tempest_c_adapter(tempest-boot-check "${TEMPEST_PORT}/android/bootstrap_platform.c")
set(registry "FEX_MODULE(bootstrap_platform)\n")
foreach(source IN LISTS TEMPEST_RUNTIME)
    get_filename_component(stem "${source}" NAME_WE)
    if(stem MATCHES "^runtime_(audio|pad|fiber)$")
        continue()
    endif()
    tempest_c_adapter(tempest-boot-check "${source}")
    string(APPEND registry "FEX_MODULE(${stem})\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tempest/runtime_registry.inc" "${registry}")
target_include_directories(tempest-boot-check PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tempest")
target_compile_definitions(tempest-boot-check PRIVATE _DEFAULT_SOURCE TEMPEST_FEX)
target_link_libraries(tempest-boot-check PRIVATE tempest-fex-bridge tempest-atrac9)
if(ANDROID)
    add_executable(tempest-vulkan-check "${TEMPEST_PORT}/android/vulkan_check.c")
    target_compile_options(tempest-vulkan-check PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(tempest-vulkan-check PRIVATE vulkan)
endif()
option(TEMPEST_ANDROID_RENDERER "Build Tempest's renderer for Android" OFF)
if(TEMPEST_ANDROID_RENDERER)
    add_subdirectory("${TEMPEST_PORT}/gpu" tempest-gpu)
    target_compile_definitions(tempest_gpu PRIVATE TEMPEST_FEX)
    target_include_directories(tempest_gpu PRIVATE "${TEMPEST_PORT}/android" "${TEMPEST_BRIDGE}")
    target_link_libraries(tempest_gpu PRIVATE CodeEmitter FEXCore FEXHeaderUtils)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/android-libs")
    add_subdirectory("${TEMPEST_PORT}/out/android-deps/adrenotools" adrenotools EXCLUDE_FROM_ALL)
    add_library(main SHARED "${TEMPEST_PORT}/android/app_main.cpp"
        "${TEMPEST_PORT}/android/boot_check.cpp" "${TEMPEST_PORT}/android/fex_runtime.cpp")
    tempest_c_adapter(main "${TEMPEST_PORT}/android/app_platform.c")
    set(app_registry "FEX_MODULE(app_platform)\n")
    foreach(source IN LISTS TEMPEST_RUNTIME)
        get_filename_component(stem "${source}" NAME_WE)
        if(stem STREQUAL "runtime_fiber")
            continue()
        endif()
        tempest_c_adapter(main "${source}")
        string(APPEND app_registry "FEX_MODULE(${stem})\n")
    endforeach()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tempest-app")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tempest-app/runtime_registry.inc" "${app_registry}")
    target_include_directories(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/tempest-app")
    target_compile_definitions(main PRIVATE _DEFAULT_SOURCE TEMPEST_FEX TEMPEST_ANDROID_ACTIVITY)
    target_link_libraries(main PRIVATE tempest_gpu tempest-fex-bridge tempest-atrac9 adrenotools android log dl)
    # The statically linked decoder/JIT are private to this JNI library. This also lets
    # ARM64 FFmpeg assembly reference its internal tables without symbol interposition.
    target_link_options(main PRIVATE "-Wl,--exclude-libs,ALL")
    add_dependencies(main main_hook hook_impl file_redirect_hook gsl_alloc_hook)
    add_executable(tempest-srt-check "${TEMPEST_PORT}/tests/test_srt_arm64.cpp"
        "${TEMPEST_PORT}/tests/test_gpu_runtime_stubs.cpp")
    target_compile_features(tempest-srt-check PRIVATE cxx_std_23)
    target_include_directories(tempest-srt-check PRIVATE "${TEMPEST_PORT}/gpu/shim" "${TEMPEST_PORT}/gpu/shadps4")
    target_link_libraries(tempest-srt-check PRIVATE tempest_gpu tempest-fex-bridge)
endif()
