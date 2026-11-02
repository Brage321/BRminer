#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace brminer::warthog {

struct LaunchConfiguration {
    std::uint32_t threads{}, batch_size{67108864}, block_size{256};
    bool operator==(const LaunchConfiguration& other) const {
        return threads == other.threads && batch_size == other.batch_size && block_size == other.block_size;
    }
};

struct CalibrationConstraints {
    std::optional<std::uint32_t> threads, batch_size, block_size;
};

struct TuningControls {
    bool autotune{}, retune{};
    bool full_calibration{};
    std::string profile_directory, compiler_library, backend;
    std::optional<std::uint32_t> cuda_batch_size, cuda_block_size;
};

} // namespace brminer::warthog
