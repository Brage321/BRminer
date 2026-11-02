#include "warthog_cuda.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int runCudaTests(int argc, char** argv) {
    using namespace brminer::warthog;
    const char* nvrtc{std::getenv("BRMINER_NVRTC_DLL")};
    if (!nvrtc || !*nvrtc) {
        std::cout << "BRMINER_NVRTC_DLL unset; CUDA kernel test skipped.\n";
        return 0;
    }
    if (CudaSha256tMiner::devices(nvrtc).empty()) {
        std::cout << "No NVIDIA CUDA GPU detected; CUDA kernel test skipped.\n";
        return 0;
    }

    const std::uint32_t block_size{argc >= 4 &&
        std::string_view{argv[1]} == "--bench" ?
        static_cast<std::uint32_t>(std::stoul(argv[3])) : 128U};
    CudaSha256tMiner miner{0, nvrtc, block_size};
    auto verify = [&](Header header, std::uint32_t start, std::uint32_t count,
                      std::uint32_t threshold) {
        auto actual{miner.scan(header, start, count, threshold)};
        std::sort(actual.begin(), actual.end(), [](const auto& a, const auto& b) {
            return a.nonce < b.nonce;
        });
        std::vector<Sha256tCandidate> expected;
        for (std::uint32_t offset{}; offset < count; ++offset) {
            const std::uint32_t nonce{start + offset};
            header[76] = static_cast<std::uint8_t>(nonce);
            header[77] = static_cast<std::uint8_t>(nonce >> 8);
            header[78] = static_cast<std::uint8_t>(nonce >> 16);
            header[79] = static_cast<std::uint8_t>(nonce >> 24);
            const auto hash{sha256t(header)};
            const std::uint32_t first{
                (static_cast<std::uint32_t>(hash[0]) << 24) |
                (static_cast<std::uint32_t>(hash[1]) << 16) |
                (static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
            if (first >= 0x0147ae14U && first < threshold)
                expected.push_back({nonce, first});
        }
        if (actual.size() != expected.size())
            throw std::runtime_error("CUDA SHA256t candidate count differs from CPU.");
        for (std::size_t i{}; i < actual.size(); ++i) {
            if (actual[i].nonce != expected[i].nonce ||
                actual[i].sha_word != expected[i].sha_word)
                throw std::runtime_error("CUDA SHA256t candidate differs from CPU.");
        }
    };

    Header header{};
    verify(header, 0, 256, UINT32_MAX);
    verify(header, 0x12340000U, 512, 0x80000000U);
    header[0] = 1;
    header[19] = 0xa5;
    verify(header, 0xfffff000U, 512, UINT32_MAX);
    verify(header, 0x23450000U, 8192, UINT32_MAX); // Overflow and retry.
    if (argc >= 2 && std::string_view{argv[1]} == "--bench") {
        const std::uint32_t count{argc >= 3 ?
            static_cast<std::uint32_t>(std::stoul(argv[2])) : 16777216U};
        constexpr std::uint32_t threshold{0x08000000U};
        const int batches{static_cast<int>(2013265920U / count)};
        std::size_t found{};
        const auto start_time{std::chrono::steady_clock::now()};
        for (int i{}; i < batches; ++i)
            found += miner.scan(header, static_cast<std::uint32_t>(i) * count,
                                count, threshold).size();
        const double seconds{std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count()};
        std::cout << "CUDA scan MH/s=" << (static_cast<double>(count) * batches / seconds / 1e6)
                  << " candidates=" << found << " seconds=" << seconds
                  << " block_size=" << block_size << ".\n";
    }
    std::cout << "CUDA SHA256t kernel passed on " << miner.deviceName() << ".\n";
    return 0;
}

int main(int argc, char** argv) {
    try { return runCudaTests(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
