#include "calibration.hpp"
#include "brminer_build.hpp"
#include "config.hpp"
#include "platform.hpp"
#include "quick_calibration.hpp"

#include <crypto/sha256.h>
#include <univalue.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>

namespace brminer::warthog {
namespace {

constexpr double kValidationSeconds{300}, kWarmupSeconds{30};
constexpr std::size_t kRepeats{2};

std::string hex(const std::array<unsigned char, 32>& bytes) {
    std::ostringstream out;
    for (const auto byte : bytes) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return out.str();
}

std::string hashText(const std::string& text) {
    std::array<unsigned char, 32> digest{};
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(text.data()), text.size()).Finalize(digest.data());
    return hex(digest);
}

std::string hashFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("Cannot fingerprint compiler file: " + path.string());
    CSHA256 hasher;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        hasher.Write(reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Could not read compiler file: " + path.string());
    std::array<unsigned char, 32> digest{};
    hasher.Finalize(digest.data());
    return hex(digest);
}

std::uint32_t integer(const std::string& text) {
    std::uint32_t value{};
    const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid integer: " + text);
    return value;
}

bool validConfiguration(const LaunchConfiguration& value) {
    return value.threads >= 1 && value.threads <= 1024 && value.batch_size >= 1048576 &&
        value.batch_size <= 67108864 && (value.batch_size & (value.batch_size - 1)) == 0 &&
        value.block_size >= 64 && value.block_size <= 512 && (value.block_size & (value.block_size - 1)) == 0;
}

bool satisfies(const LaunchConfiguration& value, const CalibrationConstraints& fixed) {
    return (!fixed.threads || value.threads == *fixed.threads) &&
        (!fixed.batch_size || value.batch_size == *fixed.batch_size) &&
        (!fixed.block_size || value.block_size == *fixed.block_size);
}

LaunchConfiguration defaults(const CalibrationConstraints& fixed) {
    return {fixed.threads.value_or(std::min(1024U, std::max(1U, std::thread::hardware_concurrency()))),
        fixed.batch_size.value_or(67108864), fixed.block_size.value_or(256)};
}

UniValue configurationJson(const LaunchConfiguration& config) {
    UniValue object{UniValue::VOBJ};
    object.pushKV("threads", config.threads);
    object.pushKV("batch_size", config.batch_size);
    object.pushKV("block_size", config.block_size);
    return object;
}

UniValue constraintsJson(const CalibrationConstraints& fixed) {
    UniValue object{UniValue::VOBJ};
    object.pushKV("threads", fixed.threads ? UniValue{*fixed.threads} : UniValue{});
    object.pushKV("batch_size", fixed.batch_size ? UniValue{*fixed.batch_size} : UniValue{});
    object.pushKV("block_size", fixed.block_size ? UniValue{*fixed.block_size} : UniValue{});
    return object;
}

std::filesystem::path profileDirectory(const TuningControls& controls) {
    if (!controls.profile_directory.empty()) return controls.profile_directory;
    if (const auto folder{environmentValue("BRMINER_PROFILE_DIR")}; !folder.empty()) return folder;
#if defined(_WIN32)
    if (const auto folder{environmentValue("LOCALAPPDATA")}; !folder.empty()) return std::filesystem::path{folder} / "BRminer/profiles";
#else
    if (const auto folder{environmentValue("XDG_CACHE_HOME")}; !folder.empty()) return std::filesystem::path{folder} / "brminer/profiles";
    if (const auto folder{environmentValue("HOME")}; !folder.empty()) return std::filesystem::path{folder} / ".cache/brminer/profiles";
#endif
    return executableDirectory() / "profiles";
}

UniValue hardwareIdentity(const std::string& compiler, const CalibrationConstraints& fixed) {
    auto devices{CudaSha256tMiner::identities(compiler)};
    if (devices.empty()) throw std::runtime_error("No CUDA devices available for calibration.");
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) { return a.uuid < b.uuid; });
    NvidiaMonitor monitor;
    const auto monitored{monitor.sample()};
    std::string driver;
    for (const auto& device : monitored) if (!device.driver.empty()) { driver = device.driver; break; }
    UniValue identity{UniValue::VOBJ};
    identity.pushKV("cpu", cpuDescription());
    identity.pushKV("logical_threads", std::thread::hardware_concurrency());
    identity.pushKV("os", operatingSystem());
    identity.pushKV("build_id", BRMINER_BUILD_ID);
    identity.pushKV("driver_release", driver);
    identity.pushKV("compiler_sha256", hashFile(compiler));
    const int version{devices.front().compiler_version};
    const int major{version / 1000}, minor{version % 1000 / 10};
#if defined(_WIN32)
    const auto builtin_name{"nvrtc-builtins64_" + std::to_string(major) + std::to_string(minor) + ".dll"};
#else
    const auto builtin_name{"libnvrtc-builtins.so." + std::to_string(major) + "." + std::to_string(minor)};
#endif
    identity.pushKV("builtins_sha256", hashFile(std::filesystem::path{compiler}.parent_path() / builtin_name));
    identity.pushKV("manual_constraints", constraintsJson(fixed));
    identity.pushKV("simd_disabled", environmentValue("BRMINER_DISABLE_SIMD") == "1");
    UniValue gpu_ids{UniValue::VARR};
    for (const auto& device : devices) {
        UniValue gpu{UniValue::VOBJ};
        gpu.pushKV("name", device.name);
        gpu.pushKV("uuid", device.uuid);
        gpu.pushKV("compute_capability", device.compute_capability);
        gpu.pushKV("driver_api", device.driver_version);
        gpu.pushKV("compiler_version", device.compiler_version);
        gpu_ids.push_back(std::move(gpu));
    }
    identity.pushKV("gpus", std::move(gpu_ids));
    return identity;
}

std::vector<double> numbers(const UniValue& array) {
    if (!array.isArray()) throw std::runtime_error("Invalid validation evidence.");
    std::vector<double> result;
    for (const auto& value : array.getValues()) {
        if (!value.isNum()) throw std::runtime_error("Invalid validation number.");
        const auto number{value.get_real()};
        if (!std::isfinite(number)) throw std::runtime_error("Non-finite validation number.");
        result.push_back(number);
    }
    return result;
}

ValidationEvidence evidenceFromJson(const UniValue& json) {
    ValidationEvidence evidence;
    evidence.baseline_hps = numbers(json["baseline_hps"]);
    evidence.candidate_hps = numbers(json["candidate_hps"]);
    evidence.baseline_seconds = numbers(json["baseline_seconds"]);
    evidence.candidate_seconds = numbers(json["candidate_seconds"]);
    evidence.correctness_passed = json["correctness_passed"].get_bool();
    evidence.unstable = json["unstable"].get_bool();
    evidence.thermal_limited = json["thermal_limited"].get_bool();
    return evidence;
}

bool validEvidence(const ValidationEvidence& evidence) {
    if (!evidence.correctness_passed || evidence.unstable || evidence.thermal_limited ||
        evidence.baseline_hps.size() != kRepeats || evidence.candidate_hps.size() != kRepeats ||
        evidence.baseline_seconds.size() != kRepeats || evidence.candidate_seconds.size() != kRepeats) return false;
    for (std::size_t i{}; i < kRepeats; ++i)
        if (!std::isfinite(evidence.baseline_hps[i]) || evidence.baseline_hps[i] <= 0 ||
            !std::isfinite(evidence.candidate_hps[i]) || evidence.candidate_hps[i] <= 0 ||
            !std::isfinite(evidence.baseline_seconds[i]) || evidence.baseline_seconds[i] < kValidationSeconds - .1 ||
            !std::isfinite(evidence.candidate_seconds[i]) || evidence.candidate_seconds[i] < kValidationSeconds - .1) return false;
    const auto [low, high]{std::minmax_element(evidence.baseline_hps.begin(), evidence.baseline_hps.end())};
    return *high / *low <= 1.05;
}

UniValue evidenceJson(const ValidationEvidence& evidence) {
    UniValue json{UniValue::VOBJ};
    const auto field = [&](const char* key, const std::vector<double>& values) {
        UniValue array{UniValue::VARR};
        for (const auto value : values) array.push_back(UniValue{value});
        json.pushKV(key, std::move(array));
    };
    field("baseline_hps", evidence.baseline_hps);
    field("candidate_hps", evidence.candidate_hps);
    field("baseline_seconds", evidence.baseline_seconds);
    field("candidate_seconds", evidence.candidate_seconds);
    json.pushKV("correctness_passed", evidence.correctness_passed);
    json.pushKV("unstable", evidence.unstable);
    json.pushKV("thermal_limited", evidence.thermal_limited);
    return json;
}

bool thermalLimited(const BenchmarkResult& result) {
    for (const auto& sample : result.samples) for (const auto& gpu : sample.gpus)
        if (gpu.clock_reasons && (*gpu.clock_reasons & (32U | 64U))) return true;
    return false;
}

bool unstable(const BenchmarkResult& result) {
    return result.cancelled || !result.correctness_passed || result.mean_janus_hps <= 0 ||
        result.stddev_janus_hps / result.mean_janus_hps > .10;
}

BenchmarkResult measure(const LaunchConfiguration& config, const std::string& compiler,
                        double seconds, double warmup, const std::filesystem::path& folder,
                        const std::string& label) {
    std::cout << label << ": " << config.threads << " CPU threads, CUDA batch " << config.batch_size
              << ", block " << config.block_size << " | " << seconds << " measured seconds\n" << std::flush;
    BenchmarkOptions options;
    options.duration_seconds = seconds;
    options.warmup_seconds = warmup;
    options.threads = config.threads;
    options.cuda_batch_size = config.batch_size;
    options.cuda_block_size = config.block_size;
    options.compiler_library = compiler;
    options.backend = "cuda";
    options.quiet = true;
    auto result{runBenchmark(options)};
    if (result.cancelled) throw CalibrationCancelled{};
    writeTextAtomically(folder / (label + ".json"), benchmarkJson(result));
    std::cout << "  Janus " << formatHashrate(result.mean_janus_hps)
              << " | sample deviation " << 100.0 * result.stddev_janus_hps / result.mean_janus_hps << "%\n" << std::flush;
    return result;
}

} // namespace

bool repeatableImprovement(const ValidationEvidence& evidence) {
    if (!validEvidence(evidence)) return false;
    double improvement{};
    for (std::size_t i{}; i < kRepeats; ++i) {
        const double ratio{evidence.candidate_hps[i] / evidence.baseline_hps[i]};
        if (ratio < 1.01) return false;
        improvement += ratio;
    }
    return improvement / kRepeats >= 1.015;
}

std::optional<LaunchConfiguration> validatedProfile(const std::string& text,
    const std::string& expected_identity, const CalibrationConstraints& constraints) {
    try {
        UniValue json;
        if (text.size() > 1024 * 1024 || !json.read(text) || !json.isObject() ||
            json["schema"].get_str() != "brminer.calibration.v1" ||
            json["hardware_key"].get_str() != expected_identity) return std::nullopt;
        const auto& selected{json["selected"]};
        LaunchConfiguration config{selected["threads"].getInt<std::uint32_t>(),
            selected["batch_size"].getInt<std::uint32_t>(), selected["block_size"].getInt<std::uint32_t>()};
        if (!validConfiguration(config) || !satisfies(config, constraints)) return std::nullopt;
        const auto evidence{evidenceFromJson(json["validation"])};
        if (!validEvidence(evidence)) return std::nullopt;
        if (json["defaults_retained"].get_bool()) {
            if (!(config == defaults(constraints))) return std::nullopt;
        } else if (!repeatableImprovement(evidence)) return std::nullopt;
        return config;
    } catch (const std::exception&) { return std::nullopt; }
}

CalibrationResolution resolveCalibration(const std::string& compiler,
    const CalibrationConstraints& fixed, const TuningControls& controls) {
    CalibrationResolution resolution;
    resolution.configuration = defaults(fixed);
    if (!validConfiguration(resolution.configuration)) throw std::invalid_argument("Invalid calibration constraints.");
    if (fixed.threads && fixed.batch_size && fixed.block_size) {
        resolution.defaults_retained = true;
        resolution.message = "All launch settings are manual; calibration is not needed.";
        return resolution;
    }
    const auto folder{profileDirectory(controls)};
    if (!controls.autotune && !controls.retune && !std::filesystem::is_directory(folder)) {
        resolution.defaults_retained = true;
        resolution.message = "No calibration profile; using defaults. Run --calibrate to tune this hardware.";
        return resolution;
    }
    const auto identity{hardwareIdentity(compiler, fixed)};
    const bool cache_allowed{!identity["driver_release"].get_str().empty()};
    if (!cache_allowed && controls.full_calibration)
        throw std::runtime_error("Driver release identity is unavailable; use quick calibration without a persistent validated profile.");
    const auto key{hashText(identity.write())};
    resolution.profile = folder / (key + ".json");
    if (cache_allowed && !controls.retune && std::filesystem::is_regular_file(resolution.profile) &&
        std::filesystem::file_size(resolution.profile) <= 1024 * 1024) {
        std::ifstream input{resolution.profile};
        const std::string text{std::istreambuf_iterator<char>{input}, {}};
        if (const auto cached{validatedProfile(text, key, fixed)}) {
            resolution.configuration = *cached;
            resolution.cached = true;
            resolution.defaults_retained = *cached == defaults(fixed);
            resolution.message = "Loaded calibrated hardware profile: " + resolution.profile.string();
            return resolution;
        }
    }
    if (!controls.autotune && !controls.retune) {
        resolution.defaults_retained = true;
        resolution.message = "No matching valid calibration profile; using defaults.";
        return resolution;
    }

    std::cout << "Quick calibration measures Verus, GPU SHA/PCIe capacity, then combined filter balance.\n"
              << "Manual settings stay fixed. Short-run choices remain provisional until full validation.\n" << std::flush;
    const auto stamp{std::chrono::system_clock::now().time_since_epoch().count()};
    const auto report{folder / "runs" / (key.substr(0, 16) + "-" + std::to_string(stamp))};
    std::filesystem::create_directories(report);
    const auto baseline{resolution.configuration};
    const auto quick{quickCalibration(compiler, fixed, report)};
    if (!controls.full_calibration) {
        resolution.configuration = quick.selected;
        resolution.initial_candidate_fraction = quick.initial_candidate_fraction;
        resolution.calibrated = true;
        resolution.defaults_retained = quick.selected == baseline;
        resolution.profile.clear();
        resolution.message = "Quick calibration completed in " + std::to_string(static_cast<unsigned>(std::ceil(quick.elapsed_seconds))) +
            " s. Provisional plan only; no validated profile saved. Report: " + quick.report.string() +
            ". Use --calibrate --full for repeated five-minute validation and profile saving.";
        return resolution;
    }
    const auto finalists{quick.finalists};
    std::cout << "Full validation: two 300-second measured runs per finalist and defaults, after 30-second warmup each.\n" << std::flush;
    std::vector<BenchmarkResult> baseline_runs;
    std::vector<std::vector<BenchmarkResult>> candidate_runs(finalists.size());
    std::vector<bool> failed(finalists.size(), false);
    for (std::size_t repeat{}; repeat < kRepeats; ++repeat) {
        const auto baseline_run = [&] {
            baseline_runs.push_back(measure(baseline, compiler, kValidationSeconds, kWarmupSeconds, report,
                "validate-default-" + std::to_string(repeat + 1)));
        };
        if (repeat == 0) baseline_run();
        for (std::size_t order{}; order < finalists.size(); ++order) {
            const auto index{repeat == 0 ? order : finalists.size() - order - 1};
            if (failed[index]) continue;
            try {
                candidate_runs[index].push_back(measure(finalists[index], compiler, kValidationSeconds,
                    kWarmupSeconds, report, "validate-candidate-" + std::to_string(index + 1) + "-" + std::to_string(repeat + 1)));
            } catch (const CalibrationCancelled&) { throw; }
            catch (const std::exception& error) {
                failed[index] = true;
                std::cout << "  Candidate rejected: " << error.what() << '\n' << std::flush;
            }
        }
        if (repeat != 0) baseline_run();
    }
    const auto evidence_for = [&](const std::vector<BenchmarkResult>& candidates) {
        ValidationEvidence evidence;
        evidence.correctness_passed = true;
        for (const auto& result : baseline_runs) {
            evidence.baseline_hps.push_back(result.mean_janus_hps);
            evidence.baseline_seconds.push_back(result.measured_s);
            evidence.correctness_passed &= result.correctness_passed && !result.cancelled;
            evidence.unstable |= unstable(result);
            evidence.thermal_limited |= thermalLimited(result);
        }
        for (const auto& result : candidates) {
            evidence.candidate_hps.push_back(result.mean_janus_hps);
            evidence.candidate_seconds.push_back(result.measured_s);
            evidence.correctness_passed &= result.correctness_passed && !result.cancelled;
            evidence.unstable |= unstable(result);
            evidence.thermal_limited |= thermalLimited(result);
        }
        return evidence;
    };
    auto selected_evidence{evidence_for(baseline_runs)};
    resolution.defaults_retained = true;
    double winning_mean{};
    UniValue comparisons{UniValue::VARR};
    for (std::size_t i{}; i < finalists.size(); ++i) {
        const auto evidence{evidence_for(candidate_runs[i])};
        const bool improved{!failed[i] && repeatableImprovement(evidence)};
        UniValue comparison{UniValue::VOBJ};
        comparison.pushKV("configuration", configurationJson(finalists[i]));
        comparison.pushKV("validation", evidenceJson(evidence));
        comparison.pushKV("repeatable_improvement", improved);
        comparisons.push_back(std::move(comparison));
        if (improved) {
            const double mean{std::accumulate(evidence.candidate_hps.begin(), evidence.candidate_hps.end(), 0.0) / kRepeats};
            if (mean > winning_mean) {
                winning_mean = mean;
                resolution.configuration = finalists[i];
                resolution.defaults_retained = false;
                selected_evidence = evidence;
            }
        }
    }
    writeTextAtomically(report / "comparisons.json", comparisons.write(2));
    resolution.calibrated = true;
    if (!validEvidence(selected_evidence)) {
        resolution.configuration = baseline;
        resolution.defaults_retained = true;
        resolution.profile.clear();
        resolution.message = "Calibration was inconclusive or unstable; defaults retained and no new profile saved. Evidence: " + report.string();
        return resolution;
    }
    if (hashText(hardwareIdentity(compiler, fixed).write()) != key)
        throw std::runtime_error("Hardware or compiler changed during calibration; no profile saved.");
    UniValue profile{UniValue::VOBJ};
    profile.pushKV("schema", "brminer.calibration.v1");
    profile.pushKV("hardware_key", key);
    profile.pushKV("identity", identity);
    profile.pushKV("selected", configurationJson(resolution.configuration));
    profile.pushKV("defaults_retained", resolution.defaults_retained);
    profile.pushKV("validation", evidenceJson(selected_evidence));
    profile.pushKV("evidence_directory", report.string());
    if (!validatedProfile(profile.write(), key, fixed)) throw std::runtime_error("Generated profile failed validation; no profile saved.");
    writeTextAtomically(resolution.profile, profile.write(2) + '\n');
    resolution.message = std::string{resolution.defaults_retained ? "No repeatable gain; defaults retained. Profile: " : "Validated calibration saved: "} + resolution.profile.string();
    return resolution;
}

int runCalibrationCommand(int argc, char** argv) {
    CalibrationConstraints fixed;
    TuningControls controls;
    controls.autotune = controls.retune = std::string{argv[1]} != "--calibration-status";
    const auto from_env = [&](const char* name, std::optional<std::uint32_t>& value) {
        const auto text{environmentValue(name)};
        if (!text.empty()) value = integer(text);
    };
    from_env("BRMINER_THREADS", fixed.threads);
    from_env("BRMINER_CUDA_BATCH_SIZE", fixed.batch_size);
    from_env("BRMINER_CUDA_BLOCK_SIZE", fixed.block_size);
    for (int i{2}; i < argc; ++i) {
        const std::string option{argv[i]};
        if (option == "--full") { controls.full_calibration = true; continue; }
        if (option == "--help") {
            std::cout << "brminer --calibrate [--full] [--threads N] [--cuda-batch-size N] [--cuda-block-size N]\n"
                "    [--nvrtc-library PATH] [--profile-dir DIRECTORY]\n"
                "Omitted settings are tuned. Quick calibration produces a provisional session plan.\n"
                "--full confirms finalists with repeated five-minute runs and saves a validated profile.\n"
                "--calibration-status checks cached settings without tuning.\n";
            return 0;
        }
        if (i + 1 == argc) throw std::invalid_argument("Missing value after " + option);
        const std::string value{argv[++i]};
        if (option == "--threads" || option == "--cpu_threads" || option == "-t") fixed.threads = parseThreadCount(value);
        else if (option == "--cuda-batch-size") fixed.batch_size = integer(value);
        else if (option == "--cuda-block-size") fixed.block_size = integer(value);
        else if (option == "--nvrtc-library") controls.compiler_library = value;
        else if (option == "--profile-dir") controls.profile_directory = value;
        else throw std::invalid_argument("Unknown calibration option: " + option);
    }
    const auto compilers{controls.compiler_library.empty() ? nvrtcLibraries() : std::vector<std::string>{controls.compiler_library}};
    std::string compiler, last_error;
    for (const auto& candidate : compilers) try {
        if (!CudaSha256tMiner::identities(candidate).empty()) { compiler = candidate; break; }
    } catch (const std::exception& error) { last_error = error.what(); }
    if (compiler.empty()) throw std::runtime_error("Calibration requires an available CUDA compiler and GPU. " + last_error);
    const auto resolution{resolveCalibration(compiler, fixed, controls)};
    std::cout << resolution.message << '\n' << "Configuration: " << resolution.configuration.threads
        << " threads, batch " << resolution.configuration.batch_size << ", block " << resolution.configuration.block_size << '\n';
    return 0;
}

} // namespace brminer::warthog
