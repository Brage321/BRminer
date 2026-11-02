#pragma once

#include <filesystem>
#include <string>

namespace brminer {

class DynamicLibrary {
public:
    DynamicLibrary() = default;
    explicit DynamicLibrary(const std::string& path, bool system = false);
    ~DynamicLibrary();
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    void* symbol(const char* name) const;
    explicit operator bool() const { return handle != nullptr; }
private:
    void* handle{};
};

std::filesystem::path executableDirectory();
std::string cpuDescription();
std::string operatingSystem();
std::string environmentValue(const char* name);
void writeTextAtomically(const std::filesystem::path& path, const std::string& text);

} // namespace brminer
