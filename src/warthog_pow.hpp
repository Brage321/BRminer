#pragma once

#include <array>
#include <cstdint>

namespace brminer::warthog {

using Header = std::array<std::uint8_t, 80>;

struct PowResult {
	bool sha256t_candidate{false};
	bool meets_target{false};
	bool meets_share_target{false};
};

std::array<std::uint8_t, 32> sha256t(const Header& header);
PowResult checkJanusHash(const Header& header, double share_difficulty = 0.0);
PowResult checkJanusHash(const Header& header, const std::array<std::uint8_t, 32>& sha_hash,
						 double share_difficulty = 0.0);

}  // namespace brminer::warthog
