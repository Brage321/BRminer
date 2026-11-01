#pragma once

#include <string_view>

namespace brminer::warthog {

int runMiner(std::string_view payout_address, std::string_view thread_setting,
             std::string_view node_setting);

}  // namespace brminer::warthog
