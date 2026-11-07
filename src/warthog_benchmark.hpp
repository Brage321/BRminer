#pragma once

#include "warthog_console.hpp"
#include "warthog_cuda.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace brminer::warthog {

struct BenchmarkOptions {
    double duration_seconds{300}, warmup_seconds{30};
    std::uint32_t threads{}, cuda_batch_size{4194304}, cuda_block_size{256};
    std::string compiler_library;
    std::string backend{"auto"};
    bool quiet{};
    double initial_candidate_fraction{0.00005};
};

struct BenchmarkSample {
    double elapsed_s{}, interval_s{}, cpu_sha_hps{}, gpu_sha_hps{}, verus_hps{}, janus_hps{};
    double gpu_queue_wait_pct{};
    std::uint64_t queue_depth{};
    std::uint32_t gpu_filter_threshold{};
    double gpu_filter_floor_fraction{};
    std::vector<GpuTelemetry> gpus;
};

struct QueueInstrumentationStats {
    std::uint64_t consumer_lock_wait_nanos{}, consumer_lock_hold_nanos{};
    std::uint64_t consumer_lock_acquisitions{}, empty_checks{}, fast_empty_checks{}, batches_taken{};
    std::uint64_t producer_lock_wait_nanos{}, producer_lock_hold_nanos{};
    std::uint64_t producer_lock_acquisitions{}, producer_full_wait_nanos{};
    std::uint64_t high_water_candidates{};
};

struct FilterControllerStats {
    std::uint64_t adjustments{};
    std::uint32_t minimum_threshold{}, maximum_threshold{};
};

struct BenchmarkResult {
    BenchmarkOptions options;
    std::string cpu, os, build_id, backend, compiler_library, cuda_pipeline{"sync"};
    std::string affinity_policy{"none"};
    std::vector<std::string> affinity_assignments;
    std::string queue_policy{"deque"};
    std::string filter_policy{"current"};
    std::vector<std::string> gpu_names;
    std::vector<CudaDeviceIdentity> cuda_devices;
    std::vector<GpuTelemetry> devices;
    std::vector<BenchmarkSample> samples;
    CudaPipelineStats cuda_pipeline_stats;
    QueueInstrumentationStats queue_stats;
    FilterControllerStats filter_stats;
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
