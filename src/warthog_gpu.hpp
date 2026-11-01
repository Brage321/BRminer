#pragma once

#include "warthog_pow.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brminer::warthog {

struct Sha256tCandidate {
	std::uint32_t nonce{};
	std::array<std::uint8_t, 32> hash{};
};

class OpenClSha256tMiner {
public:
	static std::vector<std::string> devices();

	explicit OpenClSha256tMiner(std::size_t device_index);
	~OpenClSha256tMiner();
	OpenClSha256tMiner(const OpenClSha256tMiner&) = delete;
	OpenClSha256tMiner& operator=(const OpenClSha256tMiner&) = delete;

	std::vector<Sha256tCandidate> scan(const Header& header, std::uint32_t nonce_start,
									  std::uint32_t nonce_count,
									  std::uint32_t candidate_threshold = UINT32_MAX);
	const std::string& deviceName() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

}  // namespace brminer::warthog
