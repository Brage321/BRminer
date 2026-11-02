#include "warthog_benchmark.hpp"
#include "brminer_build.hpp"
#include "config.hpp"
#include "platform.hpp"

#include <univalue.h>

#include <cmath>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace brminer::warthog {
namespace {

double number(const std::string& text) {
    std::size_t end{};
    const double value{std::stod(text, &end)};
    if (end != text.size() || !std::isfinite(value)) throw std::invalid_argument("Invalid numeric option: " + text);
    return value;
}

std::uint32_t integer(const std::string& text) {
    std::uint32_t value{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid integer option: " + text);
    return value;
}

UniValue gpuJson(const GpuTelemetry& gpu) {
    UniValue json{UniValue::VOBJ};
    json.pushKV("name", gpu.name);
    json.pushKV("uuid", gpu.uuid);
    json.pushKV("driver", gpu.driver);
    const auto field = [&](const char* key, const std::optional<double>& value) {
        json.pushKV(key, value ? UniValue{*value} : UniValue{});
    };
    field("temperature_c", gpu.temperature_c);
    field("utilization_pct", gpu.utilization_pct);
    field("power_w", gpu.power_w);
    field("core_mhz", gpu.core_mhz);
    field("memory_mhz", gpu.memory_mhz);
    json.pushKV("clock_reasons", gpu.clock_reasons ? UniValue{*gpu.clock_reasons} : UniValue{});
    json.pushKV("clock_reason_text", clockReasonText(gpu.clock_reasons));
    return json;
}

} // namespace

void summarizeBenchmark(BenchmarkResult& result) {
    result.cpu = cpuDescription();
    result.os = operatingSystem();
    result.build_id = BRMINER_BUILD_ID;
    result.measured_s = 0;
    double work{};
    for (const auto& sample : result.samples) {
        result.measured_s += sample.interval_s;
        work += sample.janus_hps * sample.interval_s;
    }
    if (result.measured_s == 0) return;
    result.mean_janus_hps = work / result.measured_s;
    double variance{}, squared_weights{};
    for (const auto& sample : result.samples) {
        variance += sample.interval_s * std::pow(sample.janus_hps - result.mean_janus_hps, 2);
        squared_weights += sample.interval_s * sample.interval_s;
    }
    result.stddev_janus_hps = std::sqrt(variance / result.measured_s);
    // Descriptive sample error only; calibration also requires independent repeats.
    const double effective_samples{result.measured_s * result.measured_s / squared_weights};
    result.standard_error_hps = result.stddev_janus_hps / std::sqrt(effective_samples);
}

std::string benchmarkJson(const BenchmarkResult& result) {
    UniValue json{UniValue::VOBJ};
    json.pushKV("schema", "brminer.benchmark.v1");
    json.pushKV("workload", "warthog-header-sequence-v1");
    json.pushKV("miner_version", BRMINER_VERSION);
    json.pushKV("build_id", result.build_id);
    json.pushKV("os", result.os);
    json.pushKV("cpu", result.cpu);
    json.pushKV("backend", result.backend);
    json.pushKV("compiler_library", result.compiler_library);
    json.pushKV("correctness_passed", result.correctness_passed);
    json.pushKV("cancelled", result.cancelled);
    json.pushKV("threads", result.options.threads);
    json.pushKV("cuda_batch_size", result.options.cuda_batch_size);
    json.pushKV("cuda_block_size", result.options.cuda_block_size);
    json.pushKV("initial_candidate_fraction", result.options.initial_candidate_fraction);
    json.pushKV("warmup_seconds", result.options.warmup_seconds);
    json.pushKV("requested_seconds", result.options.duration_seconds);
    json.pushKV("elapsed_seconds", result.elapsed_s);
    json.pushKV("measured_seconds", result.measured_s);
    json.pushKV("mean_janus_hps", result.mean_janus_hps);
    json.pushKV("stddev_janus_hps", result.stddev_janus_hps);
    json.pushKV("sample_standard_error_hps", result.standard_error_hps);
    UniValue devices{UniValue::VARR};
    for (const auto& device : result.devices) devices.push_back(gpuJson(device));
    json.pushKV("gpu_monitoring", std::move(devices));
    UniValue identities{UniValue::VARR};
    for (const auto& device : result.cuda_devices) {
        UniValue identity{UniValue::VOBJ};
        identity.pushKV("name", device.name);
        identity.pushKV("uuid", device.uuid);
        identity.pushKV("compute_capability", device.compute_capability);
        identity.pushKV("driver_version", device.driver_version);
        identity.pushKV("compiler_version", device.compiler_version);
        identities.push_back(std::move(identity));
    }
    json.pushKV("cuda_devices", std::move(identities));
    UniValue samples{UniValue::VARR};
    for (const auto& sample : result.samples) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("elapsed_s", sample.elapsed_s);
        item.pushKV("interval_s", sample.interval_s);
        item.pushKV("cpu_sha_hps", sample.cpu_sha_hps);
        item.pushKV("gpu_sha_hps", sample.gpu_sha_hps);
        item.pushKV("verus_hps", sample.verus_hps);
        item.pushKV("janus_hps", sample.janus_hps);
        item.pushKV("gpu_queue_wait_pct", sample.gpu_queue_wait_pct);
        item.pushKV("queue_depth", sample.queue_depth);
        UniValue monitoring{UniValue::VARR};
        for (const auto& gpu : sample.gpus) monitoring.push_back(gpuJson(gpu));
        item.pushKV("gpu_monitoring", std::move(monitoring));
        samples.push_back(std::move(item));
    }
    json.pushKV("samples", std::move(samples));
    return json.write(2) + '\n';
}

int runBenchmarkCommand(int argc, char** argv) {
    BenchmarkOptions options;
    std::string output{"benchmark.json"};
    if (const auto threads{environmentValue("BRMINER_THREADS")}; !threads.empty())
        options.threads = parseThreadCount(threads);
    if (const auto batch{environmentValue("BRMINER_CUDA_BATCH_SIZE")}; !batch.empty())
        options.cuda_batch_size = integer(batch);
    if (const auto block{environmentValue("BRMINER_CUDA_BLOCK_SIZE")}; !block.empty())
        options.cuda_block_size = integer(block);
    for (int i{2}; i < argc; ++i) {
        const std::string option{argv[i]};
        if (option == "--quiet") { options.quiet = true; continue; }
        if (i + 1 == argc) throw std::invalid_argument("Missing value after " + option);
        const std::string value{argv[++i]};
        if (option == "--duration") options.duration_seconds = number(value);
        else if (option == "--warmup") options.warmup_seconds = number(value);
        else if (option == "--threads" || option == "--cpu_threads" || option == "-t") options.threads = parseThreadCount(value);
        else if (option == "--cuda-batch-size") options.cuda_batch_size = integer(value);
        else if (option == "--cuda-block-size") options.cuda_block_size = integer(value);
        else if (option == "--nvrtc-library" || option == "--nvrtc-dll") options.compiler_library = value;
        else if (option == "--backend") options.backend = value;
        else if (option == "--json") output = value;
        else throw std::invalid_argument("Unknown benchmark option: " + option);
    }
    const auto result{runBenchmark(options)};
    const std::filesystem::path path{output};
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path};
    if (!file || !(file << benchmarkJson(result))) throw std::runtime_error("Cannot write benchmark JSON: " + output);
    std::cout << "Measured Janus: " << formatHashrate(result.mean_janus_hps)
              << " over " << result.measured_s << " seconds; JSON: " << output << '\n';
    return result.cancelled ? 130 : 0;
}

} // namespace brminer::warthog
