cmake_minimum_required(VERSION 3.22)
project(BRminer VERSION 1.0.0 LANGUAGES C CXX)

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
include(cmake/apply_janus_msvc_compat.cmake)
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
string(REPLACE "uint pos = 2 * atomic_inc(out) + 1;" "uint pos = 9 * atomic_inc(out) + 1;" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "if (pos < 2 * M) {" "if (pos < 9 * M) {" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
string(REPLACE "out[pos + 0] = gid;\n            out[pos + 1] = v0;" "out[pos + 0] = gid;\n            out[pos + 1] = v0;\n            out[pos + 2] = v1;\n            out[pos + 3] = v2;\n            out[pos + 4] = v3;\n            out[pos + 5] = v4;\n            out[pos + 6] = v5;\n            out[pos + 7] = v6;\n            out[pos + 8] = v7;" WARTHOG_OPENCL_KERNEL_SOURCE "${WARTHOG_OPENCL_KERNEL_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/warthog_gpu_kernel.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/warthog_gpu_kernel.hpp" @ONLY)

add_library(janus_verushash STATIC
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.cpp"
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verus_clhash_port.cpp"
)
target_compile_features(janus_verushash PUBLIC cxx_std_20)
target_include_directories(janus_verushash PUBLIC
    "${janusminer_source_SOURCE_DIR}/src/shared/src"
    "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash"
)

add_executable(brminer
    src/main.cpp
    src/connectcoin_pool.cpp
    src/warthog_stratum.cpp
    src/warthog_pow.cpp
    src/warthog_gpu.cpp
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
    core_interface
    connectcoin_node
    connectcoin_common
    connectcoin_util
    connectcoin_crypto
    univalue
    janus_verushash
)
if(WIN32)
    target_link_libraries(brminer PRIVATE ws2_32)
endif()
if(MINGW)
    target_link_options(brminer PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    add_executable(warthog-pow-tests tests/warthog_pow_tests.cpp src/warthog_pow.cpp)
    target_compile_features(warthog-pow-tests PRIVATE cxx_std_20)
    target_include_directories(warthog-pow-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${connectcoin_source_SOURCE_DIR}/src"
    )
    target_link_libraries(warthog-pow-tests PRIVATE connectcoin_crypto janus_verushash)
    add_test(NAME warthog-pow-tests COMMAND warthog-pow-tests)

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
    target_link_libraries(warthog-gpu-tests PRIVATE connectcoin_crypto janus_verushash)
    add_test(NAME warthog-gpu-tests COMMAND warthog-gpu-tests)

    add_executable(config-tests tests/config_tests.cpp)
    target_compile_features(config-tests PRIVATE cxx_std_17)
    add_test(NAME config-tests COMMAND config-tests)

    add_executable(pow-tests tests/pow_tests.cpp)
    target_compile_features(pow-tests PRIVATE cxx_std_17)
    target_include_directories(pow-tests PRIVATE "${connectcoin_source_SOURCE_DIR}/src/randomx/src")
    target_link_libraries(pow-tests PRIVATE randomx)
    add_test(NAME pow-tests COMMAND pow-tests)
endif()
