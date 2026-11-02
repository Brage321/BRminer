#pragma once

#include "warthog_console.hpp"
#include "warthog_cuda.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace brminer::warthog {

struct BenchmarkOptions {
    double duration_seconds{300}, warmup_seconds{30};
    std::uint32_t threads{}, cuda_batch_size{67108864}, cuda_block_size{256};
    std::string compiler_library;
    std::string backend{"auto"};
    bool quiet{};
    double initial_candidate_fraction{0.00005};
};

struct BenchmarkSample {
    double elapsed_s{}, interval_s{}, cpu_sha_hps{}, gpu_sha_hps{}, verus_hps{}, janus_hps{};
    double gpu_queue_wait_pct{};
    std::uint64_t queue_depth{};
    std::vector<GpuTelemetry> gpus;
};

struct BenchmarkResult {
    BenchmarkOptions options;
    std::string cpu, os, build_id, backend, compiler_library;
    std::vector<std::string> gpu_names;
    std::vector<CudaDeviceIdentity> cuda_devices;
    std::vector<GpuTelemetry> devices;
    std::vector<BenchmarkSample> samples;
    double elapsed_s{}, measured_s{}, mean_janus_hps{}, stddev_janus_hps{}, standard_error_hps{};
    bool correctness_passed{}, cancelled{};
};

BenchmarkResult runBenchmark(const BenchmarkOptions& options);
std::string benchmarkJson(const BenchmarkResult& result);
int runBenchmarkCommand(int argc, char** argv);
void summarizeBenchmark(BenchmarkResult& result);
Header benchmarkFixtureHeader(std::uint64_t epoch = 0);
void verifyCudaBenchmarkWorkload(CudaSha256tMiner& miner);
void verifyCpuBenchmarkWorkload();

} // namespace brminer::warthog
