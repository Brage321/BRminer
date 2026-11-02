#include "platform.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace brminer {

DynamicLibrary::DynamicLibrary(const std::string& path, bool system) {
#if defined(_WIN32)
    const auto requested{std::filesystem::path{path}};
    const auto file{!system && requested.has_parent_path() ? std::filesystem::absolute(requested) : requested};
    handle = LoadLibraryExW(file.wstring().c_str(), nullptr, system ? LOAD_LIBRARY_SEARCH_SYSTEM32 :
        (file.has_parent_path() ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS :
         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
    (void)system;
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle) throw std::runtime_error("Could not load library: " + path);
}

DynamicLibrary::~DynamicLibrary() {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle{std::exchange(other.handle, nullptr)} {}
DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) { DynamicLibrary previous{std::move(*this)}; handle = std::exchange(other.handle, nullptr); }
    return *this;
}

void* DynamicLibrary::symbol(const char* name) const {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::array<wchar_t, 32768> path{};
    const auto count{GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()))};
    if (count == 0 || count >= path.size()) throw std::runtime_error("Cannot locate the miner executable.");
    return std::filesystem::path{path.data()}.parent_path();
#else
    std::array<char, 4096> path{};
    const auto count{readlink("/proc/self/exe", path.data(), path.size() - 1)};
    if (count < 0) throw std::runtime_error("Cannot locate the miner executable.");
    return std::filesystem::path{std::string{path.data(), static_cast<std::size_t>(count)}}.parent_path();
#endif
}

std::string cpuDescription() {
#if defined(_WIN32)
    std::array<char, 256> value{};
    DWORD size{static_cast<DWORD>(value.size())};
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "ProcessorNameString", RRF_RT_REG_SZ, nullptr, value.data(), &size) == ERROR_SUCCESS) {
        std::string text{value.data()};
        const auto end{text.find_last_not_of(' ')};
        return text.substr(0, end == std::string::npos ? 0 : end + 1);
    }
#else
    std::ifstream input{"/proc/cpuinfo"};
    for (std::string line; std::getline(input, line); ) {
        if (line.rfind("model name", 0) != 0) continue;
        const auto colon{line.find(':')};
        if (colon == std::string::npos) continue;
        const auto begin{line.find_first_not_of(" \t", colon + 1)};
        if (begin != std::string::npos) return line.substr(begin);
    }
#endif
    return "Unknown CPU";
}

std::string operatingSystem() {
#if defined(_WIN32)
    return "windows-x86_64";
#else
    return "linux-x86_64";
#endif
}

std::string environmentValue(const char* name) {
    const char* value{std::getenv(name)};
    return value ? value : "";
}

void writeTextAtomically(const std::filesystem::path& path, const std::string& text) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const auto suffix{std::chrono::steady_clock::now().time_since_epoch().count()};
    const std::filesystem::path temporary{path.string() + ".tmp-" + std::to_string(suffix)};
    {
        std::ofstream stream{temporary, std::ios::binary};
        if (!stream || !(stream << text) || !stream.flush())
            throw std::runtime_error("Cannot write file: " + temporary.string());
    }
#if defined(_WIN32)
    const bool replaced{MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0};
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    const bool replaced{!error};
#endif
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Cannot replace file: " + path.string());
    }
}

} // namespace brminer
