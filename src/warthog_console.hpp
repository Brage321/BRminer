#pragma once

#include "gpu_monitor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace brminer::warthog {

inline std::string formatHashrate(double rate) {
    if (!std::isfinite(rate) || rate < 0) rate = 0;
    constexpr std::array<const char*, 5> units{"H/s", "kH/s", "MH/s", "GH/s", "TH/s"};
    std::size_t unit{};
    while (rate >= 1000 && unit + 1 < units.size()) { rate /= 1000; ++unit; }
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << rate << ' ' << units[unit];
    return out.str();
}

inline std::string formatUptime(double seconds) {
    const auto total{static_cast<std::uint64_t>(std::max(0.0, seconds))};
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << total / 3600 << ':'
        << std::setw(2) << total / 60 % 60 << ':' << std::setw(2) << total % 60;
    return out.str();
}

struct MiningStats {
    double uptime{}, janus{}, janus_average{}, gpu_sha{}, cpu_sha{}, verus{}, difficulty{};
    std::uint64_t accepted{}, rejected{}, blocks{};
    std::vector<GpuTelemetry> gpus;
};

inline std::string formatSensor(std::optional<double> value, const char* unit, int precision = 0) {
    if (!value || !std::isfinite(*value)) return "N/A";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << *value << unit;
    return out.str();
}

inline std::string formatMiningTable(const MiningStats& stats, const std::string& backend,
                                    std::uint32_t threads, int columns = 96) {
    std::ostringstream out;
    const bool wide{columns >= 96};
    const int count{wide ? 5 : 3};
    const int width{wide ? 16 : std::max(16, (columns - 1) / 3 - 3)};
    const int table_width{1 + count * (width + 3)};
    const auto border = [&] {
        out << '+' << std::string(table_width - 2, '-') << "+\n";
    };
    const auto row = [&](const std::initializer_list<std::string>& cells) {
        const int cell_count{static_cast<int>(cells.size())};
        const int content_width{table_width - 1 - 3 * cell_count};
        int index{};
        out << '|';
        for (const auto& cell : cells) {
            const int cell_width{content_width / cell_count + (index++ < content_width % cell_count)};
            out << ' ' << std::left << std::setw(cell_width) << cell.substr(0, cell_width) << " |";
        }
        out << '\n';
    };
    std::ostringstream difficulty, interval, rejected;
    if (stats.difficulty > 0) {
        difficulty << std::setprecision(4) << std::defaultfloat << stats.difficulty;
    } else difficulty << "waiting";
    if (stats.difficulty > 0 && stats.janus > 0) {
        const double seconds{stats.difficulty / stats.janus};
        interval << '~' << std::fixed << std::setprecision(1);
        if (seconds >= 3600) interval << seconds / 3600 << " h";
        else if (seconds >= 60) interval << seconds / 60 << " min";
        else interval << seconds << " s";
    } else interval << "waiting";
    const double submissions{static_cast<double>(stats.accepted) + static_cast<double>(stats.rejected)};
    rejected << stats.rejected << " (" << std::fixed << std::setprecision(1)
             << (submissions > 0 ? 100.0 * stats.rejected / submissions : 0.0) << "%)";
    out << "\nWarthog | " << backend << " | " << threads << " CPU threads | Uptime "
        << formatUptime(stats.uptime) << '\n';
    border();
    if (wide) {
        row({"JANUS (10s)", "JANUS AVG", "GPU SHA256t", "CPU Verus"});
        row({formatHashrate(stats.janus), formatHashrate(stats.janus_average),
             formatHashrate(stats.gpu_sha), formatHashrate(stats.verus)});
        border();
        row({"Accepted", "Rejected", "Blocks", "Share difficulty", "Est. share time"});
        row({std::to_string(stats.accepted), rejected.str(), std::to_string(stats.blocks),
             difficulty.str(), interval.str()});
    } else {
        row({"JANUS (10s)", "JANUS AVG", "GPU SHA256t"});
        row({formatHashrate(stats.janus), formatHashrate(stats.janus_average), formatHashrate(stats.gpu_sha)});
        border();
        row({"CPU Verus", "Share difficulty"});
        row({formatHashrate(stats.verus), difficulty.str()});
        border();
        row({width >= 19 ? "Accepted / Rejected" : "Accept / Reject", "Blocks", "Est. share time"});
        row({std::to_string(stats.accepted) + " / " + rejected.str(),
             std::to_string(stats.blocks), interval.str()});
    }
    border();
    if (!stats.gpus.empty()) {
        if (wide) row({"GPU sensors", "Temperature", "Utilization", "Power", "Core clock"});
        else row({"GPU sensors", "Temperature / Use", "Power / Core clock"});
        for (std::size_t i{}; i < stats.gpus.size(); ++i) {
            const auto& gpu{stats.gpus[i]};
            const auto label{"GPU " + std::to_string(i)};
            if (wide) row({label, formatSensor(gpu.temperature_c, " C"),
                formatSensor(gpu.utilization_pct, "%"), formatSensor(gpu.power_w, " W", 1),
                formatSensor(gpu.core_mhz, " MHz")});
            else row({label, formatSensor(gpu.temperature_c, " C") + " / " + formatSensor(gpu.utilization_pct, "%"),
                formatSensor(gpu.power_w, " W") + " / " + formatSensor(gpu.core_mhz, " MHz")});
        }
        border();
        for (std::size_t i{}; i < stats.gpus.size(); ++i)
            out << "GPU " << i << " clock limits: " << clockReasonText(stats.gpus[i].clock_reasons)
                << " | Memory clock: " << formatSensor(stats.gpus[i].memory_mhz, " MHz") << '\n';
    } else if (backend != "CPU") out << "GPU monitoring: N/A\n";
    return out.str();
}

// Owns all mining output so background events cannot overwrite the dashboard.
class MiningConsole {
public:
    MiningConsole();
    ~MiningConsole();
    MiningConsole(const MiningConsole&) = delete;
    MiningConsole& operator=(const MiningConsole&) = delete;
    void start(std::string backend, std::uint32_t threads, bool diagnostics = false, bool quiet = false);
    void update(const MiningStats& stats);
    void shares(std::uint64_t accepted, std::uint64_t rejected, std::uint64_t blocks);
    void event(const std::string& message);
    void diagnostic(const std::string& line);
    bool isLive() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace brminer::warthog
