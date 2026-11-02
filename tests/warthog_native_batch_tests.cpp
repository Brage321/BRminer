#include "../src/warthog_native_batch/warthog_native_batch.hpp"

#include <crypto/verushash/verushash.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::uint32_t byteSwap(std::uint32_t value) {
    return (value << 24) | ((value & 0x0000ff00U) << 8) |
        ((value & 0x00ff0000U) >> 8) | (value >> 24);
}

void checkJob(std::uint32_t seed, std::size_t count) {
    std::array<std::uint8_t, 80> header{};
    for (std::size_t i{}; i < 76; ++i) {
        seed = seed * 1664525U + 1013904223U;
        header[i] = static_cast<std::uint8_t>(seed >> 24);
    }
    std::vector<std::uint32_t> nonces(count);
    for (std::size_t i{}; i < count; ++i) {
        seed = seed * 1664525U + 1013904223U;
        nonces[i] = byteSwap(seed);
    }
    if (count > 0) nonces[0] = 0;
    if (count > 1) nonces[1] = byteSwap(0x01020304U);
    if (count > 2) nonces[2] = UINT32_MAX;
    if (count > 3) nonces[3] = nonces[1];
    std::vector<std::uint8_t> result(count * 32);
    if (brminer_native_batch_hash(header.data(), nonces.data(), count, result.data()) != 0) {
        throw std::runtime_error("Native paired Verus batch hash failed.");
    }
    for (std::size_t i{}; i < count; ++i) {
        const auto nonce{byteSwap(nonces[i])};
        header[76] = static_cast<std::uint8_t>(nonce);
        header[77] = static_cast<std::uint8_t>(nonce >> 8);
        header[78] = static_cast<std::uint8_t>(nonce >> 16);
        header[79] = static_cast<std::uint8_t>(nonce >> 24);
        const auto reference{verus_hash(std::span<const std::uint8_t>{header}, true)};
        if (std::memcmp(result.data() + i * 32, reference.data(), 32) != 0) {
            throw std::runtime_error("Native paired Verus hash differs from consensus.");
        }
    }
}

void benchmarkBatchHash() {
    constexpr std::size_t workers{14};
    constexpr std::size_t batch_size{1024};
    std::array<std::uint8_t, 76> header{};
    for (std::size_t i{}; i < header.size(); ++i) header[i] = static_cast<std::uint8_t>(i * 17);
    header[68] = header[69] = header[70] = 0;
    header[71] = 3;
    std::barrier start_line{workers + 1};
    std::atomic<bool> stop{};
    std::array<std::uint64_t, workers> counts{};
    std::array<std::uint64_t, workers> checksums{};
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t worker{}; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            std::array<std::uint32_t, batch_size> nonces{};
            std::array<std::uint8_t, batch_size * 32> hashes{};
            std::uint64_t count{};
            std::uint64_t checksum{};
            start_line.arrive_and_wait();
            while (!stop.load(std::memory_order_relaxed)) {
                for (std::size_t i{}; i < batch_size; ++i) {
                    nonces[i] = byteSwap(static_cast<std::uint32_t>(
                        worker * 0x10000000ULL + count + i));
                }
                if (brminer_native_batch_hash(header.data(), nonces.data(),
                        batch_size, hashes.data()) != 0) std::terminate();
                checksum += hashes[0];
                count += batch_size;
            }
            counts[worker] = count;
            checksums[worker] = checksum;
        });
    }
    const auto begin{std::chrono::steady_clock::now()};
    start_line.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::seconds{30});
    stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads) thread.join();
    const double seconds{std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count()};
    std::uint64_t total{};
    std::uint64_t checksum{};
    for (std::size_t i{}; i < workers; ++i) {
        total += counts[i];
        checksum += checksums[i];
    }
    std::cout << "native_batch_raw workers=" << workers
        << " elapsed_s=" << seconds << " mhps=" << total / seconds / 1e6
        << " checksum=" << checksum << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    if (!brminer_native_cpu_supported()) {
        std::cout << "AVX2 Verus batch path unavailable on this CPU; portable fallback remains active.\n";
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        benchmarkBatchHash();
        return 0;
    }
    for (std::size_t count : std::array<std::size_t, 9>{0, 1, 2, 3, 63, 64, 65, 511, 512}) {
        checkJob(0x348acedU + static_cast<std::uint32_t>(count), count);
    }
    std::array<std::thread, 4> workers;
    for (std::size_t i{}; i < workers.size(); ++i) {
        workers[i] = std::thread{[i] {
            for (std::uint32_t job{}; job < 6; ++job) {
                checkJob(0x12345678U + static_cast<std::uint32_t>(i * 97) + job, 31 + job);
            }
        }};
    }
    for (auto& worker : workers) worker.join();
    std::cout << "Native paired Verus hashes match consensus across batches and job changes.\n";
}
