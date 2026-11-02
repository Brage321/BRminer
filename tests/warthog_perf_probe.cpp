#include "warthog_pow.hpp"

#include <block/header/custom_float.hpp>
#include <crypto/verushash/verushash.hpp>
#include <crypto/verushash/verus_constants.hpp>
#if defined(_MSC_VER)
#include "brminer_verus_opt.hpp"
#endif

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace brminer::warthog;
    using Clock = std::chrono::steady_clock;
    constexpr std::uint32_t rounds{500000};
    Header header{};
    for (std::size_t i{}; i < header.size(); ++i) header[i] = static_cast<std::uint8_t>(i * 17);
    header[68] = header[69] = header[70] = 0;
    header[71] = 2;
    constexpr std::uint32_t workers{14};
#if defined(BRMINER_NATIVE_BATCH)
    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} == "--sustained-batch-seconds") {
        const auto seconds{std::stoul(argv[2])};
        if (seconds < 1 || seconds > 3600) return 2;
        header[71] = 3;
        const std::size_t batch_size{argc == 4 ? std::stoul(argv[3]) : 1024};
        if (batch_size < 128 || batch_size > 8192) return 2;
        std::barrier start_line{workers + 1};
        std::atomic<bool> stop{};
        std::array<std::uint64_t, workers> counts{};
        std::array<std::uint64_t, workers> sums{};
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::uint32_t worker{}; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                std::vector<Sha256tCandidate> candidates(batch_size);
                std::vector<PowResult> results;
                std::uint64_t count{};
                std::uint64_t sum{};
                for (std::size_t i{}; i < batch_size; ++i) {
                    candidates[i].sha_word = 0x0147ae15U +
                        static_cast<std::uint32_t>((i * 7919U) % 0x07000000U);
                }
                start_line.arrive_and_wait();
                while (!stop.load(std::memory_order_relaxed)) {
                    for (std::size_t i{}; i < batch_size; ++i) {
                        candidates[i].nonce = static_cast<std::uint32_t>(
                            worker * 0x10000000ULL + count + i);
                    }
                    checkJanusHashFromShaWordBatch(header, candidates, 1.58173392012e12,
                        results);
                    sum += results[0].sha256t_candidate;
                    count += batch_size;
                }
                counts[worker] = count;
                sums[worker] = sum;
            });
        }
        const auto begin{Clock::now()};
        start_line.arrive_and_wait();
        std::this_thread::sleep_for(std::chrono::seconds{seconds});
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : threads) thread.join();
        std::uint64_t total{};
        for (const auto count : counts) total += count;
        std::uint64_t checksum{};
        for (const auto sum : sums) checksum += sum;
        const double elapsed{std::chrono::duration<double>(Clock::now() - begin).count()};
        std::cout << "batch_candidate workers=" << workers << " elapsed_s=" << elapsed
            << " mhps=" << total / elapsed / 1e6 << " checksum=" << checksum << '\n';
        return 0;
    }
#endif
    if (argc == 3 && std::string_view{argv[1]} == "--sustained-candidate-seconds") {
        const auto seconds{std::stoul(argv[2])};
        if (seconds < 1 || seconds > 3600) return 2;
        std::barrier start_line{workers + 1};
        std::atomic<bool> stop{};
        std::array<std::atomic<std::uint64_t>, workers> counts{};
        std::array<std::uint64_t, workers> sums{};
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::uint32_t worker{}; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                Header local{header};
                std::uint64_t count{};
                std::uint64_t sum{};
                start_line.arrive_and_wait();
                while (!stop.load(std::memory_order_relaxed)) {
                    for (std::uint32_t i{}; i < 4096; ++i) {
                        const auto nonce{static_cast<std::uint32_t>(worker * 0x10000000U + count + i)};
                        std::memcpy(local.data() + 76, &nonce, sizeof(nonce));
                        const auto result{checkJanusHashFromShaWord(local, 0x08000000U, 1.0e12)};
                        sum += result.sha256t_candidate + result.meets_target + result.meets_share_target;
                    }
                    count += 4096;
                    counts[worker].store(count, std::memory_order_relaxed);
                }
                sums[worker] = sum;
            });
        }
        const auto begin{Clock::now()};
        start_line.arrive_and_wait();
        std::uint64_t last_count{};
        auto last_time{begin};
        for (unsigned long sample = 30; sample <= seconds; sample += 30) {
            std::this_thread::sleep_until(begin + std::chrono::seconds{sample});
            std::uint64_t total{};
            for (const auto& count : counts) total += count.load(std::memory_order_relaxed);
            const auto now{Clock::now()};
            const double interval{std::chrono::duration<double>(now - last_time).count()};
            std::cout << "sustained_candidate elapsed_s=" << sample
                      << " mhps=" << (total - last_count) / interval / 1e6 << std::endl;
            last_count = total;
            last_time = now;
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : threads) thread.join();
        std::uint64_t checksum{};
        for (const auto sum : sums) checksum += sum;
        std::cout << "checksum=" << checksum << std::endl;
        return 0;
    }
    std::array<std::uint8_t, 32> sha{};
    sha[0] = 8;
    std::uint64_t checksum{};

    const auto measure = [&](const char* name, auto&& work) {
        const auto start = Clock::now();
        for (std::uint32_t i{}; i < rounds; ++i) {
            header[76] = static_cast<std::uint8_t>(i);
            header[77] = static_cast<std::uint8_t>(i >> 8);
            header[78] = static_cast<std::uint8_t>(i >> 16);
            header[79] = static_cast<std::uint8_t>(i >> 24);
            checksum += work(i);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << name << " seconds=" << seconds
                  << " mhps=" << (static_cast<double>(rounds) / seconds / 1e6) << '\n';
    };

    measure("cpu_sha256t", [&](std::uint32_t) {
        return static_cast<std::uint64_t>(sha256t(header)[0]);
    });
    measure("verushash", [&](std::uint32_t) {
        return static_cast<std::uint64_t>(Verus::VerusHasher{}
            .write(std::span<const std::uint8_t>{header.data(), header.size()})
            .finalize(false)[0]);
    });
    measure("verushash_v2_2", [&](std::uint32_t) {
        return static_cast<std::uint64_t>(Verus::VerusHasher{}
            .writeHeader80(header.data()).finalize(true)[0]);
    });
#if defined(_MSC_VER)
    alignas(32) std::array<std::uint8_t, 2 * Verus::keySizeInBytes> key{};
    for (std::size_t i{}; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 37 + (i >> 4));
    std::memcpy(key.data() + Verus::keySizeInBytes, key.data(), Verus::keyRefreshsize);
    std::memset(key.data() + Verus::keySizeInBytes + Verus::keyRefreshsize, 0,
        Verus::keySizeInBytes - Verus::keyRefreshsize);
    auto* scratch = reinterpret_cast<__m128i**>(
        key.data() + Verus::keySizeInBytes + Verus::keyRefreshsize);
    alignas(32) std::array<std::uint8_t, 64> input{};
    measure("clhash_v2_2_and_restore", [&](std::uint32_t i) {
        std::memcpy(input.data() + 32, &i, sizeof(i));
        verus_restore_mutations(scratch);
        return verusclhash_sv2_2(key.data(), input.data(), Verus::keyMask, scratch);
    });
    measure("clhash_v2_2_no_restore", [&](std::uint32_t i) {
        std::memcpy(input.data() + 32, &i, sizeof(i));
        return verusclhash_sv2_2(key.data(), input.data(), Verus::keyMask, scratch);
    });
    std::array<std::uint8_t, 32> keyed_out{};
    measure("keyed_haraka", [&](std::uint32_t i) {
        haraka512_keyed(keyed_out.data(), input.data(),
            reinterpret_cast<const u128*>(key.data()) + (i & Verus::keyMask16));
        return static_cast<std::uint64_t>(keyed_out[0]);
    });
#endif
    constexpr CustomFloat exponent{0, 3006477107ULL};
    measure("janus_math", [&](std::uint32_t i) {
        sha[1] = static_cast<std::uint8_t>(i);
        const CustomFloat sha_value{sha};
        const CustomFloat verus_value{-2, 3000000000ULL};
        return static_cast<std::uint64_t>((verus_value * pow(sha_value, exponent)).mantissa());
    });
    measure("full_gpu_candidate", [&](std::uint32_t i) {
        sha[1] = static_cast<std::uint8_t>(i);
        const auto result = checkJanusHash(header, sha, 1.58173392012e12);
        return static_cast<std::uint64_t>(result.sha256t_candidate +
            result.meets_target + result.meets_share_target);
    });
    const auto parallel = [&](const char* name, auto&& work) {
        std::barrier start_line{workers + 1};
        std::array<std::uint64_t, workers> sums{};
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::uint32_t worker{}; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                Header local{header};
                std::uint64_t sum{};
                start_line.arrive_and_wait();
                for (std::uint32_t i{}; i < rounds; ++i) {
                    const std::uint32_t nonce{worker * rounds + i};
                    std::memcpy(local.data() + 76, &nonce, sizeof(nonce));
                    sum += work(local);
                }
                sums[worker] = sum;
            });
        }
        const auto begin = Clock::now();
        start_line.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        for (const auto sum : sums) checksum += sum;
        std::cout << name << " seconds=" << seconds
                  << " mhps=" << (static_cast<double>(workers) * rounds / seconds / 1e6) << '\n';
    };
    parallel("parallel_verushash_v2_2", [](const Header& local) {
        return static_cast<std::uint64_t>(Verus::VerusHasher{}
            .writeHeader80(local.data()).finalize(true)[0]);
    });
    parallel("parallel_candidate_check", [](const Header& local) {
        const auto result{checkJanusHashFromShaWord(local, 0x08000000U, 1.0e12)};
        return static_cast<std::uint64_t>(result.sha256t_candidate +
            result.meets_target + result.meets_share_target);
    });
    std::cout << "checksum=" << checksum << '\n';
}
