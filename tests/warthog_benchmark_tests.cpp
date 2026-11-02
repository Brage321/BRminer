#include "warthog_benchmark.hpp"

#include <univalue.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

int main() {
    using namespace brminer::warthog;
    BenchmarkResult result;
    result.options.duration_seconds = 3;
    result.samples = {{1, 1, 0, 1000, 100, 400}, {3, 2, 0, 1500, 150, 700}};
    summarizeBenchmark(result);
    assert(result.measured_s == 3);
    assert(std::abs(result.mean_janus_hps - 600) < 1e-6);
    assert(result.stddev_janus_hps > 0);
    assert(std::abs(result.standard_error_hps - result.stddev_janus_hps / std::sqrt(9.0 / 5.0)) < 1e-6);
    UniValue json;
    assert(json.read(benchmarkJson(result)));
    assert(json["schema"].get_str() == "brminer.benchmark.v1");
    assert(json["samples"].size() == 2);
    BenchmarkOptions invalid;
    invalid.duration_seconds = std::numeric_limits<double>::quiet_NaN();
    bool rejected{};
    try { runBenchmark(invalid); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    BenchmarkOptions cpu;
    cpu.backend = "cpu";
    cpu.threads = 1;
    cpu.warmup_seconds = 0;
    cpu.duration_seconds = 1;
    cpu.quiet = true;
    const auto measured{runBenchmark(cpu)};
    assert(measured.correctness_passed && !measured.cancelled);
    assert(measured.backend == "CPU" && measured.gpu_names.empty());
    assert(std::abs(measured.measured_s - 1) < .01);
    assert(measured.mean_janus_hps > 0);
    assert(measured.samples.size() >= 1);
    for (const auto& sample : measured.samples) assert(sample.gpu_sha_hps == 0);
}
