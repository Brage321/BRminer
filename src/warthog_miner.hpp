#pragma once

#include <string_view>
#include "mining_settings.hpp"

namespace brminer::warthog {

int runMiner(std::string_view payout_address, std::string_view thread_setting,
             std::string_view node_setting, const TuningControls& tuning = {});

}  // namespace brminer::warthog
