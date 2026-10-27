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
include(cmake/apply_connectcoin_dev_fee.cmake)
target_include_directories(connectcoin_node PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(brminer
    src/main.cpp
    "${connectcoin_source_SOURCE_DIR}/src/init/bitcoind.cpp"
)
target_compile_features(brminer PRIVATE cxx_std_20)
target_include_directories(brminer PRIVATE
    "${connectcoin_source_SOURCE_DIR}/src"
    "${connectcoin_source_SOURCE_DIR}/src/randomx/src"
    "${connectcoin_source_BINARY_DIR}"
    "${connectcoin_source_BINARY_DIR}/src"
)
target_link_libraries(brminer PRIVATE
    core_interface
    connectcoin_node
    connectcoin_common
    connectcoin_util
)
if(MINGW)
    target_link_options(brminer PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    add_executable(config-tests tests/config_tests.cpp)
    target_compile_features(config-tests PRIVATE cxx_std_17)
    add_test(NAME config-tests COMMAND config-tests)

    add_executable(pow-tests tests/pow_tests.cpp)
    target_compile_features(pow-tests PRIVATE cxx_std_17)
    target_include_directories(pow-tests PRIVATE "${connectcoin_source_SOURCE_DIR}/src/randomx/src")
    target_link_libraries(pow-tests PRIVATE randomx)
    add_test(NAME pow-tests COMMAND pow-tests)
endif()
