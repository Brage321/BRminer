#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brminer {

struct GpuTelemetry {
    std::string name, uuid, driver;
    std::optional<double> temperature_c, utilization_pct, power_w, core_mhz, memory_mhz;
    std::optional<std::uint64_t> clock_reasons;
};

std::string clockReasonText(std::optional<std::uint64_t> reasons);

class NvidiaMonitor {
public:
    NvidiaMonitor();
    ~NvidiaMonitor();
    NvidiaMonitor(const NvidiaMonitor&) = delete;
    NvidiaMonitor& operator=(const NvidiaMonitor&) = delete;
    std::vector<GpuTelemetry> sample() const;
    bool available() const;
    const std::string& unavailableReason() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace brminer
