#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace brminer::warthog {

using Header = std::array<std::uint8_t, 80>;

struct Sha256tCandidate {
	std::uint32_t nonce{};
	std::uint32_t sha_word{};
};

struct PowResult {
	bool sha256t_candidate{false};
	bool meets_target{false};
	bool meets_share_target{false};
};

std::array<std::uint8_t, 32> sha256t(const Header& header);
PowResult checkJanusHash(const Header& header, double share_difficulty = 0.0);
PowResult checkJanusHash(const Header& header, const std::array<std::uint8_t, 32>& sha_hash,
						 double share_difficulty = 0.0);
PowResult checkJanusHashFromShaWord(const Header& header, std::uint32_t sha_word,
									 double share_difficulty = 0.0);
std::array<PowResult, 2> checkJanusHashFromShaWordPair(
	const Header& first, std::uint32_t first_sha_word,
	const Header& second, std::uint32_t second_sha_word,
	double share_difficulty = 0.0);
void checkJanusHashFromShaWordBatch(const Header& job_header,
	std::span<const Sha256tCandidate> candidates, double share_difficulty,
	std::vector<PowResult>& results);

}  // namespace brminer::warthog
