#include "calibration.hpp"
#include "platform.hpp"

#include <univalue.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <limits>
#include <iostream>
#include <cstdlib>

#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::cerr << "Calibration check failed at line " << __LINE__ << ": " << #condition << '\n'; \
    std::exit(1); } } while (false)

namespace {
std::string profile(brminer::warthog::LaunchConfiguration config,
    const brminer::warthog::ValidationEvidence& evidence, bool retained = false) {
    UniValue json{UniValue::VOBJ}, selected{UniValue::VOBJ}, validation{UniValue::VOBJ};
    json.pushKV("schema", "brminer.calibration.v1");
    json.pushKV("hardware_key", "hardware-driver-compiler-build-identity");
    selected.pushKV("threads", config.threads);
    selected.pushKV("batch_size", config.batch_size);
    selected.pushKV("block_size", config.block_size);
    json.pushKV("selected", selected);
    json.pushKV("defaults_retained", retained);
    const auto field = [&](const char* name, const std::vector<double>& values) {
        UniValue array{UniValue::VARR};
        for (const auto value : values) array.push_back(UniValue{value});
        validation.pushKV(name, array);
    };
    field("baseline_hps", evidence.baseline_hps);
    field("candidate_hps", evidence.candidate_hps);
    field("baseline_seconds", evidence.baseline_seconds);
    field("candidate_seconds", evidence.candidate_seconds);
    validation.pushKV("correctness_passed", evidence.correctness_passed);
    validation.pushKV("unstable", evidence.unstable);
    validation.pushKV("thermal_limited", evidence.thermal_limited);
    json.pushKV("validation", validation);
    return json.write();
}

std::string quickProfile(brminer::warthog::LaunchConfiguration config,
    double initial_fraction = 0.025) {
    UniValue json{UniValue::VOBJ}, selected{UniValue::VOBJ};
    json.pushKV("schema", "brminer.quick-profile.v1");
    json.pushKV("hardware_key", "hardware-driver-compiler-build-identity");
    selected.pushKV("threads", config.threads);
    selected.pushKV("batch_size", config.batch_size);
    selected.pushKV("block_size", config.block_size);
    json.pushKV("selected", selected);
    json.pushKV("initial_candidate_fraction", initial_fraction);
    return json.write();
}
}

int main() try {
    using namespace brminer::warthog;
    const std::string identity{"hardware-driver-compiler-build-identity"};
    ValidationEvidence evidence{{100, 101}, {104, 105}, {300, 300}, {300, 300}, true, false, false};
    assert(repeatableImprovement(evidence));
    ValidationEvidence one_percent{{100, 100}, {101.1, 101.1}, {300, 300}, {300, 300}, true, false, false};
    assert(repeatableImprovement(one_percent));
    CalibrationConstraints fixed;
    fixed.threads = 14;
    assert(validatedProfile(profile({14, 16777216, 128}, evidence), identity, fixed));
    assert(!validatedProfile(profile({14, 16777216, 128}, evidence), "changed-driver-or-build", fixed));
    assert(!validatedProfile(profile({8, 16777216, 128}, evidence), identity, fixed));
    assert(!validatedProfile(profile({14, 16777216, 7}, evidence), identity, fixed));
    assert(!validatedProfile("{broken", identity, fixed));
    const auto quick{cachedQuickProfile(quickProfile({14, 4194304, 256}), identity, fixed)};
    assert((quick && quick->configuration == LaunchConfiguration{14, 4194304, 256}));
    assert(quick->initial_candidate_fraction == 0.025);
    assert(!cachedQuickProfile(quickProfile({8, 4194304, 256}), identity, fixed));
    assert(!cachedQuickProfile(quickProfile({14, 4194304, 256}), "changed", fixed));
    assert(!cachedQuickProfile(quickProfile({14, 4194304, 256}, 0.5), identity, fixed));
    auto failed{evidence};
    failed.candidate_hps = {100.5, 101.5};
    assert(!repeatableImprovement(failed));
    failed.candidate_hps = {110, 100};
    assert(!repeatableImprovement(failed));
    failed = evidence;
    failed.candidate_seconds[0] = 30;
    assert(!validatedProfile(profile({14, 16777216, 128}, failed), identity, fixed));
    failed = evidence;
    failed.correctness_passed = false;
    assert(!repeatableImprovement(failed));
    failed = evidence;
    failed.unstable = true;
    assert(!repeatableImprovement(failed));
    failed = evidence;
    failed.thermal_limited = true;
    assert(!repeatableImprovement(failed));
    failed = evidence;
    failed.baseline_hps = {100, 120};
    assert(!repeatableImprovement(failed));
    failed = evidence;
    failed.candidate_hps[0] = std::numeric_limits<double>::quiet_NaN();
    assert(!repeatableImprovement(failed));
    evidence.candidate_hps = evidence.baseline_hps;
    assert(validatedProfile(profile({14, 4194304, 256}, evidence, true), identity, fixed));
    assert(!validatedProfile(profile({14, 16777216, 128}, evidence, true), identity, fixed));

    const auto name{"brminer-profile-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json"};
    const auto path{std::filesystem::current_path() / name};
    brminer::writeTextAtomically(path, "old");
    brminer::writeTextAtomically(path, "replacement");
    std::ifstream input{path};
    std::string value;
    input >> value;
    input.close();
    assert(value == "replacement");
    std::filesystem::remove(path);
} catch (const std::exception& error) {
    std::cerr << "Calibration test error: " << error.what() << '\n';
    return 1;
}
