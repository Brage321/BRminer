#include "gpu_monitor.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main(int argc, char**) {
    brminer::NvidiaMonitor monitor;
    const auto samples{monitor.sample()};
    if (argc > 1) {
        assert(!monitor.available() && samples.empty());
        return 0;
    }
    for (const auto& gpu : samples) {
        for (const auto& value : {gpu.temperature_c, gpu.utilization_pct, gpu.power_w, gpu.core_mhz, gpu.memory_mhz})
            if (value) assert(std::isfinite(*value) && *value >= 0);
        if (gpu.utilization_pct) assert(*gpu.utilization_pct <= 100);
        std::cout << gpu.name << " | temperature=" << (gpu.temperature_c ? std::to_string(*gpu.temperature_c) : "N/A")
            << " | power=" << (gpu.power_w ? std::to_string(*gpu.power_w) : "N/A") << '\n';
    }
}
