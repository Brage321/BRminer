#include "warthog_console.hpp"

#include <cassert>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    using namespace brminer::warthog;
    const MiningStats stats{3600, 477600000, 477700000, 1050000000, 100000,
        23680000, 1581733920120, 1, 0, 1};
    auto monitored{stats};
    brminer::GpuTelemetry gpu;
    gpu.temperature_c = 79;
    gpu.utilization_pct = 98;
    gpu.core_mhz = 2760;
    gpu.memory_mhz = 8251;
    gpu.clock_reasons = 0;
    monitored.gpus.push_back(gpu);
    if (argc > 1 && std::string{argv[1]} == "--live-demo") {
        MiningConsole console;
        console.start("CUDA", 14);
        if (!console.isLive()) return 2;
        console.update(stats);
        for (int i{}; i < 50; ++i) {
            console.event("SIMULATED new job " + std::to_string(i));
            auto current{stats};
            if (i >= 10) current.gpus = monitored.gpus;
            current.uptime += i;
            console.update(current);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        console.event("SIMULATED BLOCK FOUND; submitted.");
        console.shares(2, 0, 2);
        console.event("SIMULATED BLOCK ACCEPTED.");
        std::this_thread::sleep_for(std::chrono::seconds{10});
        return 0;
    }
    const auto table{formatMiningTable(stats, "CUDA", 14)};
    assert(formatHashrate(1050000000) == "1.05 GH/s");
    assert(formatSensor(std::nullopt, " W") == "N/A");
    assert(formatSensor(0.0, " W") == "0 W");
    assert(brminer::clockReasonText(std::nullopt) == "N/A");
    assert(brminer::clockReasonText(0) == "none");
    assert(brminer::clockReasonText(4) == "power cap");
    assert(formatUptime(3600) == "01:00:00");
    assert(formatUptime(90061) == "25:01:01");
    assert(table.find("477.60 MH/s") < table.find("1.05 GH/s"));
    std::istringstream lines{table};
    for (std::string line; std::getline(lines, line); ) {
        if (!line.empty() && (line.front() == '+' || line.front() == '|'))
            assert(line.size() == 96);
    }
    const auto waiting{formatMiningTable({}, "CPU", 1)};
    const auto sensor_table{formatMiningTable(monitored, "CUDA", 14)};
    assert(sensor_table.find("GPU sensors") != std::string::npos);
    assert(sensor_table.find("N/A") != std::string::npos);
    assert(sensor_table.find("GPU 0 clock limits: none") != std::string::npos);
    assert(waiting.find("nan") == std::string::npos);
    assert(waiting.find("waiting") != std::string::npos);
    assert(waiting.find("0 (0.0%)") != std::string::npos);
    std::istringstream compact{formatMiningTable(stats, "CUDA", 14, 79)};
    for (std::string line; std::getline(compact, line); ) {
        if (!line.empty() && (line.front() == '+' || line.front() == '|'))
            assert(line.size() == 79);
    }
    if (argc > 1) std::cout << table;
}
