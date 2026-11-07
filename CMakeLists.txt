cmake_minimum_required(VERSION 3.22)
project(BRminer VERSION 1.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

FetchContent_Declare(
    connectcoin_source
    GIT_REPOSITORY https://github.com/connectcoincrypto/connectcoin.git
    GIT_TAG ff459a92dab40607cda4d92c6effdb85600ceea7
    GIT_SHALLOW FALSE
)

FetchContent_Declare(
    janusminer_source
    GIT_REPOSITORY https://github.com/CoinFuMasterShifu/janusminer.git
    GIT_TAG 8d13b2bad00bde6ae10195f30ba3bec8824fd1cb
    GIT_SHALLOW FALSE
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Link ConnectCoin Core statically into BRminer" FORCE)
set(BUILD_CONNECTCOIN_BIN OFF CACHE BOOL "Do not build the ConnectCoin launcher" FORCE)
set(BUILD_DAEMON OFF CACHE BOOL "Do not build connectcoind" FORCE)
set(BUILD_CLI OFF CACHE BOOL "Do not build connectcoin-cli" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Do not build Core's standalone test runner" FORCE)
set(BUILD_GUI OFF CACHE BOOL "Do not build the Core GUI" FORCE)
set(BUILD_GUI_TESTS OFF CACHE BOOL "Do not build Core GUI tests" FORCE)
set(BUILD_BENCH OFF CACHE BOOL "Do not build Core benchmarks" FORCE)
set(BUILD_TX OFF CACHE BOOL "Do not build Core transaction tools" FORCE)
set(BUILD_UTIL OFF CACHE BOOL "Do not build Core utility tools" FORCE)
set(BUILD_WALLET_TOOL OFF CACHE BOOL "Do not build the Core wallet tool" FORCE)
set(ENABLE_WALLET OFF CACHE BOOL "Do not build wallet support" FORCE)
set(ENABLE_IPC OFF CACHE BOOL "Do not build Core IPC support" FORCE)
set(WITH_ZMQ OFF CACHE BOOL "Do not build ZMQ support" FORCE)
set(WITH_EMBEDDED_ASMAP OFF CACHE BOOL "Do not embed the optional AS map" FORCE)
set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "Do not build RandomX helper tools" FORCE)
set(RANDOMX_INSTALL OFF CACHE BOOL "Do not install RandomX" FORCE)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
endif()

FetchContent_MakeAvailable(connectcoin_source)
FetchContent_MakeAvailable(janusminer_source)
add_library(brminer_platform STATIC src/platform.cpp src/gpu_monitor.cpp)
target_compile_features(brminer_platform PUBLIC cxx_std_20)
target_include_directories(brminer_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(brminer_platform PUBLIC advapi32)
else()
    target_link_libraries(brminer_platform PUBLIC ${CMAKE_DL_LIBS})
endif()
include(cmake/apply_janus_msvc_compat.cmake)
if(MSVC)
    find_program(BRMINER_CLANG_CL NAMES clang-cl clang-cl.exe
        HINTS "${CMAKE_CURRENT_SOURCE_DIR}/out/toolchain/clang+llvm-23.1.4-x86_64-pc-windows-msvc/bin")
endif()
include(cmake/prepare_warthog_verus_opt.cmake)
include(cmake/prepare_warthog_verus_prefix.cmake)
target_include_directories(connectcoin_node PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

file(READ "${janusminer_source_SOURCE_DIR}/src/miner/gpu/kernel.cl" WARTHOG_OPENCL_KERNEL_SOURCE)
foreach(_gpu_kernel_patch IN ITEMS
    [=[W3 = gid;]=]
    [=[uint pos = 2 * atomic_inc(out) + 1;]=]
    [=[if (pos < 2 * M) {]=]
    [=[out[pos + 0] = gid;
            out[pos + 1] = v0;]=]
    [=[if (v0 >= c && v0 < threshold) {]=]
)
    string(FIND "${WARTHOG_OPENCL_KERNEL_SOURCE}" "${_gpu_kernel_patch}" _gpu_kernel_patch_index)
    if(_gpu_kernel_patch_index EQUAL -1)
        message(FATAL_ERROR "Pinned JanusMiner OpenCL kernel does not match the expected Warthog GPU patch context")
    endif()
endforeach()
string(REPLACE "W3 = gid;" "W3 = SWAP32(gid);" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "unsigned char h1[80];" "unsigned char h1[112];" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "unsigned short h2[40];" "unsigned short h2[56];" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "uint h4[20];" "uint h4[28];" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "ulong h8[10];" "ulong h8[14];" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
set(_midstate_begin "uint W0 = SWAP32(header->h4[0]);")
set(_midstate_end "W0 = SWAP32(header->h4[16]);")
string(FIND "${WARTHOG_OPENCL_KERNEL_SOURCE}" "${_midstate_begin}" _midstate_begin_pos)
string(FIND "${WARTHOG_OPENCL_KERNEL_SOURCE}" "${_midstate_end}" _midstate_end_pos)
if(_midstate_begin_pos EQUAL -1 OR _midstate_end_pos LESS _midstate_begin_pos)
    message(FATAL_ERROR "Pinned JanusMiner OpenCL SHA256 midstate context changed")
endif()
string(SUBSTRING "${WARTHOG_OPENCL_KERNEL_SOURCE}" 0 ${_midstate_begin_pos} _midstate_head)
string(SUBSTRING "${WARTHOG_OPENCL_KERNEL_SOURCE}" ${_midstate_end_pos} -1 _midstate_tail)
string(CONCAT WARTHOG_OPENCL_KERNEL_SOURCE "${_midstate_head}" [=[
    uint W0, W1, W2, W3, W4, W5, W6, W7;
    uint W8, W9, W10, W11, W12, W13, W14, W15;
    uint v0 = header->h4[20], v1 = header->h4[21];
    uint v2 = header->h4[22], v3 = header->h4[23];
    uint v4 = header->h4[24], v5 = header->h4[25];
    uint v6 = header->h4[26], v7 = header->h4[27];
    uint s0 = v0, s1 = v1, s2 = v2, s3 = v3;
    uint s4 = v4, s5 = v5, s6 = v6, s7 = v7;

    ]=] "${_midstate_tail}")
set(WARTHOG_OPENCL_KERNEL_CHUNKS "")
string(LENGTH "${WARTHOG_OPENCL_KERNEL_SOURCE}" _kernel_length)
set(_kernel_offset 0)
while(_kernel_offset LESS _kernel_length)
    math(EXPR _kernel_remaining "${_kernel_length} - ${_kernel_offset}")
    if(_kernel_remaining GREATER 4096)
        set(_kernel_chunk_length 4096)
    else()
        set(_kernel_chunk_length ${_kernel_remaining})
    endif()
    string(SUBSTRING "${WARTHOG_OPENCL_KERNEL_SOURCE}" ${_kernel_offset} ${_kernel_chunk_length} _kernel_chunk)
    string(APPEND WARTHOG_OPENCL_KERNEL_CHUNKS "    R\"BRMINER_OPENCL(${_kernel_chunk})BRMINER_OPENCL\",\n")
    math(EXPR _kernel_offset "${_kernel_offset} + ${_kernel_chunk_length}")
endwhile()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/warthog_gpu_kernel.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/warthog_gpu_kernel.hpp" @ONLY)
file(READ src/warthog_cuda_kernel.cu WARTHOG_CUDA_KERNEL_SOURCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/warthog_cuda_kernel.cu")
set(WARTHOG_CUDA_KERNEL_CHUNKS "")
string(LENGTH "${WARTHOG_CUDA_KERNEL_SOURCE}" _cuda_kernel_length)
set(_cuda_kernel_offset 0)
while(_cuda_kernel_offset LESS _cuda_kernel_length)
    math(EXPR _cuda_kernel_remaining "${_cuda_kernel_length} - ${_cuda_kernel_offset}")
    if(_cuda_kernel_remaining GREATER 4096)
        set(_cuda_kernel_chunk_length 4096)
    else()
        set(_cuda_kernel_chunk_length ${_cuda_kernel_remaining})
    endif()
    string(SUBSTRING "${WARTHOG_CUDA_KERNEL_SOURCE}" ${_cuda_kernel_offset}
        ${_cuda_kernel_chunk_length} _cuda_kernel_chunk)
    string(APPEND WARTHOG_CUDA_KERNEL_CHUNKS
        "    R\"BRMINER_CUDA(${_cuda_kernel_chunk})BRMINER_CUDA\",\n")
    math(EXPR _cuda_kernel_offset "${_cuda_kernel_offset} + ${_cuda_kernel_chunk_length}")
endwhile()
configure_file(src/warthog_cuda_kernel.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/warthog_cuda_kernel.hpp" @ONLY)

set(_brminer_verus_simd_unit "${BRMINER_VERUS_OPT_SOURCE}")
if(BRMINER_VERUS_OPT_SOURCE AND MSVC AND BRMINER_CLANG_CL)
    set(_brminer_verus_simd_unit "${CMAKE_CURRENT_BINARY_DIR}/generated/brminer_verus_opt_clang.obj")
    add_custom_command(OUTPUT "${_brminer_verus_simd_unit}"
        COMMAND "${BRMINER_CLANG_CL}" /nologo /c /O2 -mavx2 -maes -mpclmul -mssse3 /std:c++20 /EHsc
            "/I${janusminer_source_SOURCE_DIR}/src/shared/src"
            "/Fo${_brminer_verus_simd_unit}" "${BRMINER_VERUS_OPT_SOURCE}"
        DEPENDS "${BRMINER_VERUS_OPT_SOURCE}"
        VERBATIM)
    set_source_files_properties("${_brminer_verus_simd_unit}"
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
endif()

add_library(janus_verushash STATIC
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.cpp"
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verus_clhash_port.cpp"
    ${_brminer_verus_simd_unit}
)
if(BRMINER_VERUS_OPT_SOURCE)
    target_compile_definitions(janus_verushash PRIVATE BRMINER_VERUS_SIMD=1)
    target_include_directories(janus_verushash PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    if(MSVC AND NOT BRMINER_CLANG_CL)
        set_source_files_properties("${BRMINER_VERUS_OPT_SOURCE}" PROPERTIES COMPILE_OPTIONS "/arch:AVX")
    elseif(NOT MSVC)
        set_source_files_properties("${BRMINER_VERUS_OPT_SOURCE}" PROPERTIES
            COMPILE_OPTIONS "-mavx2;-maes;-mpclmul;-mssse3")
    endif()
endif()
target_compile_features(janus_verushash PUBLIC cxx_std_20)
target_include_directories(janus_verushash PUBLIC
    "${janusminer_source_SOURCE_DIR}/src/shared/src"
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash"
)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND BRMINER_CLANG_CL)
    set(_warthog_native_batch_obj "${CMAKE_CURRENT_BINARY_DIR}/generated/warthog_native_batch_clang.obj")
    add_custom_command(OUTPUT "${_warthog_native_batch_obj}"
        COMMAND "${BRMINER_CLANG_CL}" /nologo /c /O2 -mavx2 -maes -mpclmul -mssse3
            /std:c++20 /EHsc /MT /DNDEBUG
            /DWART_NATIVE_ARCH_ID=2 /DWART_RESTORE_MODE=2 /DWART_LENGTH_HASH_MODE=1
            /DWART_REJECT_BITS=9 /DWART_SHARED_BACKUP=1 /DWART_INTERLEAVE2=1
            /DWART_FAST_MODULO=0 /DWART_NATIVE_LTCG=0
            "/I${janusminer_source_SOURCE_DIR}/src/shared/src"
            "/I${CMAKE_CURRENT_SOURCE_DIR}/src/warthog_native_batch/compat"
            "/Fo${_warthog_native_batch_obj}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/warthog_native_batch/warthog_native_batch.cpp"
        DEPENDS src/warthog_native_batch/warthog_native_batch.cpp
            src/warthog_native_batch/warthog_native_batch.hpp
            src/warthog_native_batch/generated/aes_primitives.inc
        VERBATIM)
    set_source_files_properties("${_warthog_native_batch_obj}"
        PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    add_library(warthog_native_batch STATIC "${_warthog_native_batch_obj}" src/native_cpu_features.cpp)
    target_compile_features(warthog_native_batch PRIVATE cxx_std_20)
    set_target_properties(warthog_native_batch PROPERTIES LINKER_LANGUAGE CXX)
elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    find_package(OpenSSL REQUIRED)
    add_library(warthog_native_batch STATIC src/warthog_native_batch/warthog_native_batch.cpp src/native_cpu_features.cpp)
    target_compile_features(warthog_native_batch PRIVATE cxx_std_20)
    set_source_files_properties(src/warthog_native_batch/warthog_native_batch.cpp PROPERTIES
        COMPILE_OPTIONS "-O2;-mavx2;-maes;-mpclmul;-mssse3")
    target_compile_definitions(warthog_native_batch PRIVATE WART_NATIVE_ARCH_ID=2
        WART_RESTORE_MODE=2 WART_LENGTH_HASH_MODE=1 WART_REJECT_BITS=9
        WART_SHARED_BACKUP=1 WART_INTERLEAVE2=1 WART_FAST_MODULO=0 WART_NATIVE_LTCG=0)
    target_include_directories(warthog_native_batch PRIVATE
        "${janusminer_source_SOURCE_DIR}/src/shared/src")
    target_link_libraries(warthog_native_batch PRIVATE OpenSSL::Crypto)
endif()

add_library(warthog_mining STATIC
    src/warthog_stratum.cpp
    src/warthog_console.cpp
    src/warthog_benchmark.cpp
    src/calibration.cpp
    src/quick_calibration.cpp
    src/warthog_pow.cpp
    src/warthog_gpu.cpp
    src/warthog_cuda.cpp
)
target_compile_features(warthog_mining PUBLIC cxx_std_20)
target_include_directories(warthog_mining PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(warthog_mining PRIVATE
    "${connectcoin_source_SOURCE_DIR}/src"
    "${janusminer_source_SOURCE_DIR}/thirdparty/opencl/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(warthog_mining PUBLIC brminer_platform connectcoin_crypto janus_verushash univalue)
if(TARGET warthog_native_batch)
    target_link_libraries(warthog_mining PRIVATE warthog_native_batch)
    target_compile_definitions(warthog_mining PRIVATE BRMINER_NATIVE_BATCH=1)
endif()
if(WIN32)
    target_link_libraries(warthog_mining PRIVATE ws2_32)
endif()

file(GLOB_RECURSE _brminer_identity_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cu" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_brminer_identity_sources})
list(APPEND _brminer_identity_sources "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
set(_brminer_identity "${BRminer_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE}")
foreach(_source IN LISTS _brminer_identity_sources)
    file(SHA256 "${_source}" _source_hash)
    string(APPEND _brminer_identity "${_source_hash}")
endforeach()
string(SHA256 BRMINER_BUILD_ID "${_brminer_identity}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/brminer_build.hpp"
    "#pragma once\n#define BRMINER_BUILD_ID \"${BRMINER_BUILD_ID}\"\n#define BRMINER_VERSION \"${BRminer_VERSION}\"\n")

add_executable(brminer
    src/main.cpp
    src/connectcoin_pool.cpp
    "${connectcoin_source_SOURCE_DIR}/src/init/bitcoind.cpp"
)
target_compile_features(brminer PRIVATE cxx_std_20)
set_target_properties(brminer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(brminer PRIVATE
    "${connectcoin_source_SOURCE_DIR}/src"
    "${connectcoin_source_SOURCE_DIR}/src/randomx/src"
    "${connectcoin_source_BINARY_DIR}"
    "${connectcoin_source_BINARY_DIR}/src"
    "${janusminer_source_SOURCE_DIR}/thirdparty/opencl/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(brminer PRIVATE
    warthog_mining
    brminer_platform
    core_interface
    connectcoin_node
    connectcoin_common
    connectcoin_util
    connectcoin_crypto
    univalue
    janus_verushash
)
if(TARGET warthog_native_batch)
    target_link_libraries(brminer PRIVATE warthog_native_batch)
    target_compile_definitions(brminer PRIVATE BRMINER_NATIVE_BATCH=1)
endif()
if(WIN32)
    target_link_libraries(brminer PRIVATE ws2_32)
endif()
if(MINGW)
    target_link_options(brminer PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    add_executable(warthog-perf-probe EXCLUDE_FROM_ALL
        tests/warthog_perf_probe.cpp src/warthog_pow.cpp)
    target_compile_features(warthog-perf-probe PRIVATE cxx_std_20)
    target_include_directories(warthog-perf-probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src")
    if(BRMINER_VERUS_OPT_SOURCE)
        target_include_directories(warthog-perf-probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()
    target_link_libraries(warthog-perf-probe PRIVATE connectcoin_crypto janus_verushash)
    if(TARGET warthog_native_batch)
        target_link_libraries(warthog-perf-probe PRIVATE warthog_native_batch)
        target_compile_definitions(warthog-perf-probe PRIVATE BRMINER_NATIVE_BATCH=1)
    endif()

    if(BRMINER_VERUS_OPT_SOURCE)
        add_executable(warthog-verus-opt-tests tests/warthog_verus_opt_tests.cpp)
        target_compile_features(warthog-verus-opt-tests PRIVATE cxx_std_20)
        target_include_directories(warthog-verus-opt-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(warthog-verus-opt-tests PRIVATE janus_verushash)
        if(MSVC)
            target_compile_options(warthog-verus-opt-tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(warthog-verus-opt-tests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME warthog-verus-opt-tests COMMAND warthog-verus-opt-tests)
    endif()

    if(TARGET warthog_native_batch)
        add_executable(warthog-native-batch-tests tests/warthog_native_batch_tests.cpp)
        target_compile_features(warthog-native-batch-tests PRIVATE cxx_std_20)
        target_link_libraries(warthog-native-batch-tests PRIVATE warthog_native_batch janus_verushash)
        add_test(NAME warthog-native-batch-tests COMMAND warthog-native-batch-tests)
    endif()

    add_executable(warthog-pow-tests tests/warthog_pow_tests.cpp src/warthog_pow.cpp)
    target_compile_features(warthog-pow-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-pow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src"
    )
    target_link_libraries(warthog-pow-tests PRIVATE connectcoin_crypto janus_verushash)
    add_test(NAME warthog-pow-tests COMMAND warthog-pow-tests)

    add_executable(warthog-pow-equivalence-tests
        tests/warthog_pow_equivalence_tests.cpp src/warthog_pow.cpp)
    target_compile_features(warthog-pow-equivalence-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-pow-equivalence-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src")
    target_link_libraries(warthog-pow-equivalence-tests PRIVATE connectcoin_crypto janus_verushash)
    add_test(NAME warthog-pow-equivalence-tests COMMAND warthog-pow-equivalence-tests)
    add_test(NAME warthog-portable-pow-tests COMMAND warthog-pow-tests)
    add_test(NAME warthog-portable-equivalence-tests COMMAND warthog-pow-equivalence-tests)
    set_tests_properties(warthog-portable-pow-tests warthog-portable-equivalence-tests
        PROPERTIES ENVIRONMENT "BRMINER_DISABLE_SIMD=1")

    add_executable(warthog-gpu-tests
        tests/warthog_gpu_tests.cpp
        src/warthog_gpu.cpp
        src/warthog_pow.cpp
    )
    target_compile_features(warthog-gpu-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-gpu-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src"
        "${janusminer_source_SOURCE_DIR}/thirdparty/opencl/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${janusminer_source_SOURCE_DIR}/src/shared/src"
        "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash"
    )
    target_link_libraries(warthog-gpu-tests PRIVATE connectcoin_crypto janus_verushash brminer_platform)
    add_test(NAME warthog-gpu-tests COMMAND warthog-gpu-tests)

    add_executable(warthog-cuda-tests
        tests/warthog_cuda_tests.cpp
        src/warthog_cuda.cpp
        src/warthog_gpu.cpp
        src/warthog_pow.cpp)
    target_compile_features(warthog-cuda-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-cuda-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src"
        "${janusminer_source_SOURCE_DIR}/thirdparty/opencl/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${janusminer_source_SOURCE_DIR}/src/shared/src"
        "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash")
    target_link_libraries(warthog-cuda-tests PRIVATE connectcoin_crypto janus_verushash brminer_platform)
    add_test(NAME warthog-cuda-tests COMMAND warthog-cuda-tests)

    if(TARGET warthog_native_batch)
        foreach(_warthog_pow_target IN ITEMS
                warthog-perf-probe warthog-pow-tests
                warthog-pow-equivalence-tests warthog-gpu-tests warthog-cuda-tests)
            target_link_libraries(${_warthog_pow_target} PRIVATE warthog_native_batch)
            target_compile_definitions(${_warthog_pow_target} PRIVATE BRMINER_NATIVE_BATCH=1)
        endforeach()
    endif()

    add_executable(config-tests tests/config_tests.cpp)
    target_compile_features(config-tests PRIVATE cxx_std_17)
    add_test(NAME config-tests COMMAND config-tests)

    add_executable(bounded-mpmc-ring-tests tests/bounded_mpmc_ring_tests.cpp)
    target_compile_features(bounded-mpmc-ring-tests PRIVATE cxx_std_20)
    target_include_directories(bounded-mpmc-ring-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME bounded-mpmc-ring-tests COMMAND bounded-mpmc-ring-tests)

    add_executable(warthog-console-tests tests/warthog_console_tests.cpp src/warthog_console.cpp)
    target_compile_features(warthog-console-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-console-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(warthog-console-tests PRIVATE brminer_platform)
    if(MSVC)
        target_compile_options(warthog-console-tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(warthog-console-tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME warthog-console-tests COMMAND warthog-console-tests)

    add_executable(warthog-benchmark-tests tests/warthog_benchmark_tests.cpp)
    target_link_libraries(warthog-benchmark-tests PRIVATE warthog_mining)
    add_executable(gpu-monitor-tests tests/gpu_monitor_tests.cpp)
    target_link_libraries(gpu-monitor-tests PRIVATE brminer_platform)
    add_executable(calibration-tests tests/calibration_tests.cpp)
    target_link_libraries(calibration-tests PRIVATE warthog_mining)
    foreach(_new_test IN ITEMS warthog-benchmark-tests gpu-monitor-tests calibration-tests)
        if(MSVC)
            target_compile_options(${_new_test} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${_new_test} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${_new_test} COMMAND ${_new_test})
    endforeach()
    add_test(NAME gpu-monitor-disabled-tests COMMAND gpu-monitor-tests --expect-disabled)
    set_tests_properties(gpu-monitor-disabled-tests PROPERTIES ENVIRONMENT "BRMINER_DISABLE_NVML=1")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME warthog-stratum-lifecycle-tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/stratum_lifecycle_test.py" $<TARGET_FILE:brminer>)
    set_tests_properties(warthog-stratum-lifecycle-tests PROPERTIES TIMEOUT 30)
    if(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        find_program(BRMINER_QEMU_X86 NAMES qemu-x86_64)
        if(BRMINER_QEMU_X86)
            add_test(NAME warthog-emulated-intel-no-avx
                COMMAND "${BRMINER_QEMU_X86}" -cpu Nehalem $<TARGET_FILE:warthog-pow-tests>)
            add_test(NAME warthog-emulated-amd-no-avx
                COMMAND "${BRMINER_QEMU_X86}" -cpu Opteron_G3 $<TARGET_FILE:warthog-pow-tests>)
        endif()
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pyte" RESULT_VARIABLE _has_pyte OUTPUT_QUIET ERROR_QUIET)
        if(_has_pyte EQUAL 0)
            add_test(NAME warthog-linux-terminal-tests COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_terminal_test.py" $<TARGET_FILE:warthog-console-tests>)
            set_tests_properties(warthog-linux-terminal-tests PROPERTIES TIMEOUT 25)
        endif()
    endif()

    add_executable(pow-tests tests/pow_tests.cpp)
    target_compile_features(pow-tests PRIVATE cxx_std_17)
    target_include_directories(pow-tests PRIVATE "${connectcoin_source_SOURCE_DIR}/src/randomx/src")
    target_link_libraries(pow-tests PRIVATE randomx)
    add_test(NAME pow-tests COMMAND pow-tests)

    # Keep the assert-based checks active in Release, where CMake defines NDEBUG.
    foreach(_assert_test IN ITEMS config-tests bounded-mpmc-ring-tests pow-tests warthog-pow-tests warthog-pow-equivalence-tests)
        if(MSVC)
            target_compile_options(${_assert_test} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${_assert_test} PRIVATE -UNDEBUG)
        endif()
    endforeach()
endif()
