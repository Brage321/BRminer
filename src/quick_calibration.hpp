#pragma once

#include "mining_settings.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace brminer::warthog {

struct QuickCalibrationPlan {
    LaunchConfiguration selected;
    std::vector<LaunchConfiguration> finalists;
    double initial_candidate_fraction{0.00005};
    double elapsed_seconds{}, cpu_capacity_hps{}, gpu_capacity_hps{};
    std::filesystem::path report;
};

QuickCalibrationPlan quickCalibration(const std::string& compiler,
    const CalibrationConstraints& constraints, const std::filesystem::path& report_directory);

} // namespace brminer::warthog
