#pragma once

#include "config.hpp"
#include "mining_settings.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace brminer {

struct WarthogOptions {
    std::string wallet;
    std::string threads;
    std::string pool;
    warthog::TuningControls tuning;
};

inline std::optional<WarthogOptions> parseWarthogOptions(
    const std::vector<std::string_view>& args, WarthogOptions defaults = {}) {
    bool requested{};
    for (const auto arg : args) requested |= arg == "-a" || arg == "--algo";
    if (!requested) return std::nullopt;

    for (std::size_t i{}; i < args.size(); ++i) {
        const auto option{args[i]};
        if (option == "--autotune") {
            defaults.tuning.autotune = defaults.tuning.automatic_tuning = true;
            continue;
        }
        if (option == "--no-autotune") {
            defaults.tuning.automatic_tuning = false;
            continue;
        }
        if (option == "--retune") {
            defaults.tuning.autotune = defaults.tuning.retune =
                defaults.tuning.automatic_tuning = true;
            continue;
        }
        if (i + 1 == args.size())
            throw std::invalid_argument("Missing value after " + std::string{option} + ". See --help.");
        const auto value{args[++i]};
        if (option == "-a" || option == "--algo") {
            if (value != "warthog" && value != "wart" && value != "WART")
                throw std::invalid_argument("Supported mining algorithm: warthog.");
        } else if (option == "-w" || option == "--wallet") {
            defaults.wallet = value;
        } else if (option == "-p" || option == "--pool") {
            constexpr std::string_view prefix{"stratum+tcp://"};
            defaults.pool = value.substr(value.substr(0, prefix.size()) == prefix ? prefix.size() : 0);
        } else if (option == "--cpu_threads" || option == "--cpu-threads" || option == "-t") {
            parseThreadCount(value);
            defaults.threads = value;
        } else if (option == "--profile-dir") {
            defaults.tuning.profile_directory = value;
        } else if (option == "--nvrtc-library") {
            defaults.tuning.compiler_library = value;
        } else if (option == "--backend") {
            defaults.tuning.backend = value;
        } else if (option == "--cuda-batch-size" || option == "--cuda-block-size") {
            std::uint32_t number{};
            const auto parsed{std::from_chars(value.data(), value.data() + value.size(), number)};
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                throw std::invalid_argument("Invalid CUDA launch size.");
            if (option == "--cuda-batch-size") defaults.tuning.cuda_batch_size = number;
            else defaults.tuning.cuda_block_size = number;
        } else {
            throw std::invalid_argument("Unknown mining option: " + std::string{option} + ". See --help.");
        }
    }
    parsePoolEndpoint(defaults.pool);
    return defaults;
}

} // namespace brminer
