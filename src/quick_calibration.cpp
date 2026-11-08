#include "quick_calibration.hpp"
#include "calibration.hpp"
#include "platform.hpp"
#include "warthog_benchmark.hpp"

#include <univalue.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace brminer::warthog {
namespace {
using Clock = std::chrono::steady_clock;

double cpuCapacity(std::uint32_t threads, double seconds) {
    if (threads == 0 || threads > 1024) throw std::invalid_argument("Invalid CPU calibration thread count.");
    const auto header{benchmarkFixtureHeader()};
    std::vector<Sha256tCandidate> candidates;
    for (std::uint32_t nonce{}; candidates.size() < 1024; ++nonce) {
        auto input{header};
        for (unsigned i{}; i < 4; ++i) input[76 + i] = static_cast<std::uint8_t>(nonce >> (i * 8));
        const auto hash{sha256t(input)};
        const std::uint32_t word{(static_cast<std::uint32_t>(hash[0]) << 24) |
            (static_cast<std::uint32_t>(hash[1]) << 16) | (static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
        if (word > 0x0147ae14U) candidates.push_back({nonce, word});
    }
    struct alignas(64) Counter { std::atomic<std::uint64_t> hashes{0}; };
    auto counters{std::make_unique<Counter[]>(threads)};
    std::atomic<bool> stop{false};
    std::barrier ready{static_cast<std::ptrdiff_t>(threads + 1)};
    std::vector<std::thread> workers;
    std::exception_ptr error;
    std::mutex error_mutex;
    try {
        for (std::uint32_t i{}; i < threads; ++i) workers.emplace_back([&, i] {
            ready.arrive_and_wait();
            try {
                std::vector<PowResult> results;
                while (!stop.load(std::memory_order_relaxed)) {
                    checkJanusHashFromShaWordBatch(header, candidates, 1e12, results);
                    counters[i].hashes.fetch_add(candidates.size(), std::memory_order_relaxed);
                }
            } catch (...) {
                std::lock_guard lock{error_mutex};
                error = std::current_exception();
                stop.store(true, std::memory_order_relaxed);
            }
        });
    } catch (...) {
        stop.store(true);
        for (std::size_t i{workers.size()}; i < threads; ++i) ready.arrive_and_drop();
        ready.arrive_and_wait();
        for (auto& worker : workers) worker.join();
        throw;
    }
    ready.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{750});
    const auto total = [&] {
        std::uint64_t value{};
        for (std::uint32_t i{}; i < threads; ++i) value += counters[i].hashes.load(std::memory_order_relaxed);
        return value;
    };
    const auto before{total()};
    const auto start{Clock::now()};
    std::this_thread::sleep_for(std::chrono::duration<double>{seconds});
    const auto after{total()};
    const double elapsed{std::chrono::duration<double>(Clock::now() - start).count()};
    stop.store(true);
    for (auto& worker : workers) worker.join();
    if (error) std::rethrow_exception(error);
    return static_cast<double>(after - before) / elapsed;
}

double gpuCapacity(CudaSha256tMiner& miner, std::uint32_t batch, double seconds) {
    const auto header{benchmarkFixtureHeader()};
    std::uint64_t hashes{}, next{};
    const auto start{Clock::now()};
    do {
        if (next + batch > static_cast<std::uint64_t>(UINT32_MAX) + 1) next = 0;
        miner.scan(header, static_cast<std::uint32_t>(next), batch, 0x0147ee14U);
        hashes += batch;
        next += batch;
    } while (std::chrono::duration<double>(Clock::now() - start).count() < seconds);
    return static_cast<double>(hashes) / std::chrono::duration<double>(Clock::now() - start).count();
}

UniValue configJson(const LaunchConfiguration& config) {
    UniValue json{UniValue::VOBJ};
    json.pushKV("threads", config.threads);
    json.pushKV("batch_size", config.batch_size);
    json.pushKV("block_size", config.block_size);
    return json;
}

bool thermalLimited(const BenchmarkResult& result) {
    for (const auto& sample : result.samples) for (const auto& gpu : sample.gpus)
        if (gpu.clock_reasons && (*gpu.clock_reasons & (32U | 64U))) return true;
    return false;
}
}

QuickCalibrationPlan quickCalibration(const std::string& compiler,
    const CalibrationConstraints& fixed, const std::filesystem::path& folder) {
    const auto started{Clock::now()};
    const auto hardware{std::min(1024U, std::max(1U, std::thread::hardware_concurrency()))};
    const LaunchConfiguration baseline{fixed.threads.value_or(hardware), fixed.batch_size.value_or(4194304),
        fixed.block_size.value_or(256)};
    QuickCalibrationPlan plan;
    verifyCpuBenchmarkWorkload();
    plan.selected = baseline;
    std::filesystem::create_directories(folder);
    UniValue report{UniValue::VOBJ}, cpu_probes{UniValue::VARR}, gpu_probes{UniValue::VARR}, combined{UniValue::VARR};
    report.pushKV("schema", "brminer.quick-calibration.v1");
    report.pushKV("provisional", true);
    report.pushKV("validated_profile_saved", false);
    report.pushKV("cpu", cpuDescription());
    report.pushKV("os", operatingSystem());

    std::cout << "Quick calibration 1/3: CPU Verus capacity.\n" << std::flush;
    std::vector<std::uint32_t> counts{baseline.threads};
    if (!fixed.threads) counts = {std::max(1U, hardware / 2),
        hardware > 2 ? hardware - 2 : hardware,
        hardware > 1 ? hardware - 1 : hardware, hardware};
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    std::map<std::uint32_t, double> cpu_rates;
    for (const auto count : counts) {
        const double rate{cpuCapacity(count, fixed.threads ? 5.0 : 3.0)};
        cpu_rates[count] = rate;
        UniValue probe{UniValue::VOBJ};
        probe.pushKV("threads", count);
        probe.pushKV("verus_hps", rate);
        cpu_probes.push_back(std::move(probe));
        std::cout << "  " << count << " threads: " << formatHashrate(rate) << " Verus\n" << std::flush;
    }

    std::cout << "Quick calibration 2/3: GPU SHA, CUDA launch settings, and PCIe transfers.\n" << std::flush;
    const auto identities{CudaSha256tMiner::identities(compiler)};
    std::vector<std::unique_ptr<CudaSha256tMiner>> miners;
    std::map<std::uint32_t, double> block_rates;
    const std::vector<std::uint32_t> blocks{fixed.block_size ? std::vector<std::uint32_t>{*fixed.block_size} :
        std::vector<std::uint32_t>{256, 128, 512, 64}};
    double link_capacity{};
    bool complete_bandwidth{true};
    for (std::size_t i{}; i < identities.size(); ++i) {
        auto miner{std::make_unique<CudaSha256tMiner>(i, compiler, baseline.block_size)};
        verifyCudaBenchmarkWorkload(*miner);
        gpuCapacity(*miner, 16777216, 1.0);
        UniValue gpu{UniValue::VOBJ}, probes{UniValue::VARR};
        gpu.pushKV("name", identities[i].name);
        gpu.pushKV("uuid", identities[i].uuid);
        try {
            const auto bandwidth{miner->measureBandwidth()};
            gpu.pushKV("host_to_device_bytes_s", bandwidth.host_to_device_bytes_s);
            gpu.pushKV("device_to_host_bytes_s", bandwidth.device_to_host_bytes_s);
            link_capacity += bandwidth.device_to_host_bytes_s / sizeof(Sha256tCandidate);
        } catch (const std::exception& error) {
            complete_bandwidth = false;
            gpu.pushKV("bandwidth_error", error.what());
        }
        for (const auto block : blocks) {
            try {
                miner->setBlockSize(block);
                verifyCudaBenchmarkWorkload(*miner);
                const double rate{gpuCapacity(*miner, baseline.batch_size, 1.5)};
                block_rates[block] += rate;
                UniValue probe{UniValue::VOBJ};
                probe.pushKV("block_size", block);
                probe.pushKV("sha_hps", rate);
                probes.push_back(std::move(probe));
            } catch (const std::exception& error) {
                block_rates[block] = -1e100;
                std::cout << "  Block " << block << " rejected: " << error.what() << '\n';
            }
        }
        gpu.pushKV("block_probes", std::move(probes));
        gpu_probes.push_back(std::move(gpu));
        miners.push_back(std::move(miner));
    }
    if (miners.empty()) throw std::runtime_error("Quick calibration requires a CUDA GPU.");
    const auto best_block{std::max_element(block_rates.begin(), block_rates.end(), [](const auto& a, const auto& b) { return a.second < b.second; })};
    if (best_block == block_rates.end() || best_block->second <= 0) throw std::runtime_error("No correct CUDA launch configuration.");
    std::uint32_t selected_block{best_block->first};
    if (block_rates[baseline.block_size] >= best_block->second * .99) selected_block = baseline.block_size;
    std::vector<std::uint32_t> batches{baseline.batch_size};
    if (!fixed.batch_size) {
        batches = {1048576, 2097152, 4194304, 8388608,
            16777216, 33554432, 67108864};
    }
    std::sort(batches.begin(), batches.end());
    batches.erase(std::unique(batches.begin(), batches.end()), batches.end());
    std::map<std::uint32_t, double> batch_rates;
    UniValue batch_probes{UniValue::VARR};
    for (const auto batch : batches) {
        try {
            std::array<double, 2> repetitions{};
            for (auto& repetition : repetitions) {
                for (auto& miner : miners) {
                    miner->setBlockSize(selected_block);
                    repetition += gpuCapacity(*miner, batch, 1.0);
                }
            }
            std::sort(repetitions.begin(), repetitions.end());
            const double rate{(repetitions[0] + repetitions[1]) / 2.0};
            batch_rates[batch] = rate;
            UniValue probe{UniValue::VOBJ};
            probe.pushKV("batch_size", batch);
            probe.pushKV("sha_hps", rate);
            UniValue measured{UniValue::VARR};
            for (const auto repetition : repetitions) measured.push_back(repetition);
            probe.pushKV("rates_hps", std::move(measured));
            batch_probes.push_back(std::move(probe));
        } catch (const std::exception& error) { std::cout << "  Batch " << batch << " rejected: " << error.what() << '\n'; }
    }
    const auto best_batch{std::max_element(batch_rates.begin(), batch_rates.end(), [](const auto& a, const auto& b) { return a.second < b.second; })};
    if (best_batch == batch_rates.end() || best_batch->second <= 0) throw std::runtime_error("No usable CUDA batch size.");
    std::uint32_t selected_batch{best_batch->first};
    for (const auto& [batch, rate] : batch_rates) {
        if (rate >= best_batch->second * .99) {
            selected_batch = batch;
            break;
        }
    }
    plan.gpu_capacity_hps = batch_rates[selected_batch];
    std::cout << "  GPU SHA capacity: " << formatHashrate(plan.gpu_capacity_hps) << '\n' << std::flush;
    miners.clear();

    const auto fraction = [&](std::uint32_t count) {
        double capacity{cpu_rates.at(count)};
        if (complete_bandwidth) capacity = std::min(capacity, link_capacity);
        return std::clamp(capacity * 1.02 / plan.gpu_capacity_hps, .00005, .25);
    };
    std::vector<LaunchConfiguration> configurations{baseline};
    for (const auto count : counts) {
        const LaunchConfiguration candidate{count, selected_batch, selected_block};
        if (std::find(configurations.begin(), configurations.end(), candidate) == configurations.end()) configurations.push_back(candidate);
    }
    struct Ranked { LaunchConfiguration config; double rate{}; };
    std::vector<Ranked> ranked;
    double baseline_rate{};
    bool baseline_valid{};
    std::cout << "Quick calibration 3/3: combined Janus and filter balance.\n" << std::flush;
    unsigned index{};
    const auto measure_combined = [&](const LaunchConfiguration& config, std::string_view phase) {
        BenchmarkOptions options;
        options.threads = config.threads;
        options.cuda_batch_size = config.batch_size;
        options.cuda_block_size = config.block_size;
        options.compiler_library = compiler;
        options.backend = "cuda";
        options.duration_seconds = 10;
        options.warmup_seconds = 5;
        options.initial_candidate_fraction = fraction(config.threads);
        options.quiet = true;
        try {
            const auto result{runBenchmark(options)};
            if (result.cancelled) throw CalibrationCancelled{};
            const auto filename{"quick-combined-" + std::to_string(++index) + ".json"};
            writeTextAtomically(folder / filename, benchmarkJson(result));
            const bool valid{result.correctness_passed && !thermalLimited(result) &&
                result.mean_janus_hps > 0 && result.stddev_janus_hps / result.mean_janus_hps <= .25};
            UniValue probe{UniValue::VOBJ};
            probe.pushKV("configuration", configJson(config));
            probe.pushKV("janus_hps", result.mean_janus_hps);
            probe.pushKV("correct_and_stable", valid);
            probe.pushKV("phase", std::string{phase});
            probe.pushKV("result_file", filename);
            combined.push_back(std::move(probe));
            std::cout << "  " << config.threads << " threads, batch " << config.batch_size << ", block "
                << config.block_size << ": " << formatHashrate(result.mean_janus_hps) << " Janus\n" << std::flush;
            return std::pair{valid, result.mean_janus_hps};
        } catch (const CalibrationCancelled&) { throw; }
        catch (const std::exception& error) { std::cout << "  Combined candidate rejected: " << error.what() << '\n'; }
        return std::pair{false, 0.0};
    };
    for (const auto& config : configurations) {
        const auto [valid, rate]{measure_combined(config, "screen")};
        if (config == baseline) { baseline_rate = rate; baseline_valid = valid; }
        if (valid) ranked.push_back({config, rate});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.rate > b.rate; });
    if (ranked.empty()) throw std::runtime_error("Quick calibration found no correct, stable combined configuration.");
    if (baseline_valid && !(ranked.front().config == baseline) && ranked.front().rate >= baseline_rate * 1.01) {
        const auto challenger{ranked.front()};
        std::cout << "  Confirming the short-run winner against the portable baseline.\n" << std::flush;
        // The screen starts with the baseline and reaches the winner later. Reversing that order here
        // brackets both configurations against temperature, clocks, and background-system drift.
        const auto [challenger_valid, challenger_rate]{measure_combined(challenger.config, "confirmation")};
        const auto [baseline_confirmation_valid, baseline_confirmation_rate]{measure_combined(baseline, "confirmation")};
        const bool challenger_confirmed{challenger_valid && baseline_confirmation_valid};
        if (challenger_confirmed) {
            const double challenger_average{(challenger.rate + challenger_rate) / 2.0};
            for (auto& item : ranked) {
                if (item.config == challenger.config) item.rate = challenger_average;
                if (item.config == baseline) item.rate = (item.rate + baseline_confirmation_rate) / 2.0;
            }
            baseline_rate = (baseline_rate + baseline_confirmation_rate) / 2.0;
            if (challenger_average >= baseline_rate * 1.01) plan.selected = challenger.config;
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.rate > b.rate; });
        }
    }
    if (!baseline_valid) plan.selected = ranked.front().config;
    for (const auto& item : ranked) {
        if (plan.finalists.size() >= 2) break;
        // Clearly slower short runs do not justify another ten minutes of validation.
        if (!(item.config == baseline) && (!baseline_valid || item.rate >= baseline_rate * .99))
            plan.finalists.push_back(item.config);
    }
    plan.cpu_capacity_hps = cpu_rates.at(plan.selected.threads);
    plan.initial_candidate_fraction = fraction(plan.selected.threads);
    plan.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    report.pushKV("cpu_probes", std::move(cpu_probes));
    report.pushKV("gpu_probes", std::move(gpu_probes));
    report.pushKV("batch_probes", std::move(batch_probes));
    report.pushKV("combined_probes", std::move(combined));
    report.pushKV("selected", configJson(plan.selected));
    report.pushKV("initial_candidate_fraction", plan.initial_candidate_fraction);
    report.pushKV("cpu_capacity_hps", plan.cpu_capacity_hps);
    report.pushKV("gpu_capacity_hps", plan.gpu_capacity_hps);
    report.pushKV("elapsed_seconds", plan.elapsed_seconds);
    plan.report = folder / "quick-calibration.json";
    writeTextAtomically(plan.report, report.write(2) + '\n');
    return plan;
}

} // namespace brminer::warthog
