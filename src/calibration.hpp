#pragma once

#include "mining_settings.hpp"
#include "warthog_benchmark.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace brminer::warthog {

class CalibrationCancelled : public std::runtime_error {
public:
    CalibrationCancelled() : std::runtime_error("Calibration cancelled; no new profile saved.") {}
};

struct CalibrationResolution {
    LaunchConfiguration configuration;
    std::string message;
    std::filesystem::path profile;
    bool cached{}, calibrated{}, defaults_retained{};
    double initial_candidate_fraction{0.00005};
};

struct ValidationEvidence {
    std::vector<double> baseline_hps, candidate_hps;
    std::vector<double> baseline_seconds, candidate_seconds;
    bool correctness_passed{}, unstable{}, thermal_limited{};
};

bool repeatableImprovement(const ValidationEvidence& evidence);
std::optional<LaunchConfiguration> validatedProfile(const std::string& json,
    const std::string& expected_identity, const CalibrationConstraints& constraints);
CalibrationResolution resolveCalibration(const std::string& compiler_library,
    const CalibrationConstraints& constraints, const TuningControls& controls);
int runCalibrationCommand(int argc, char** argv);

} // namespace brminer::warthog
