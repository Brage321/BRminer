#pragma once

#include <string_view>

namespace brminer::connectcoin_pool {

int runMiner(std::string_view payout_address, std::string_view thread_setting);

}  // namespace brminer::connectcoin_pool
