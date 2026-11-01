#include "warthog_pow.hpp"

#include <crypto/sha256.h>
#include <block/header/custom_float.hpp>
#include <block/header/difficulty.hpp>
#include <crypto/verushash/verushash.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace brminer::warthog {
namespace {

int comparePositive(const CustomFloat& left, const CustomFloat& right) {
	if (left.is_zero()) return right.is_zero() ? 0 : -1;
	if (right.is_zero()) return 1;
	if (left.exponent() != right.exponent()) {
		return left.exponent() < right.exponent() ? -1 : 1;
	}
	if (left.mantissa() == right.mantissa()) return 0;
	return left.mantissa() < right.mantissa() ? -1 : 1;
}

std::uint32_t packedTarget(const Header& header) {
	return (static_cast<std::uint32_t>(header[32]) << 24) |
		   (static_cast<std::uint32_t>(header[33]) << 16) |
		   (static_cast<std::uint32_t>(header[34]) << 8) |
		   static_cast<std::uint32_t>(header[35]);
}

bool belowTarget(const CustomFloat& hash, std::uint32_t encoded) {
	const std::uint32_t target_mantissa{encoded & 0x003fffffU};
	if (target_mantissa == 0) return false;
	const std::uint32_t target_zeros{encoded >> 22};
	const std::uint32_t hash_zeros{static_cast<std::uint32_t>(-hash.exponent())};
	if (target_zeros > hash_zeros) return false;
	if (target_zeros < hash_zeros) return true;
	return hash.mantissa() < (static_cast<std::uint64_t>(target_mantissa) << 10);
}

bool belowDifficulty(const CustomFloat& hash, double difficulty) {
	if (!std::isfinite(difficulty) || difficulty <= 0.0) return false;
	const TargetV2 target{difficulty};
	const std::uint32_t target_zeros{target.zeros10()};
	const std::uint32_t hash_zeros{static_cast<std::uint32_t>(-hash.exponent())};
	if (target_zeros > hash_zeros) return false;
	if (target_zeros < hash_zeros) return true;
	return hash.mantissa() < (static_cast<std::uint64_t>(target.bits22()) << 10);
}

}  // namespace

std::array<std::uint8_t, 32> sha256t(const Header& header) {
	std::array<std::uint8_t, 32> first{};
	std::array<std::uint8_t, 32> second{};
	std::array<std::uint8_t, 32> third{};
	CSHA256{}.Write(header.data(), header.size()).Finalize(first.data());
	CSHA256{}.Write(first.data(), first.size()).Finalize(second.data());
	CSHA256{}.Write(second.data(), second.size()).Finalize(third.data());
	return third;
}

PowResult checkJanusHash(const Header& header, double share_difficulty) {
	return checkJanusHash(header, sha256t(header), share_difficulty);
}

PowResult checkJanusHash(const Header& header, const std::array<std::uint8_t, 32>& sha_hash,
						 double share_difficulty) {
	const CustomFloat sha_value{sha_hash};
	constexpr CustomFloat lower_bound{-7, 2748779069ULL};
	if (comparePositive(sha_value, lower_bound) < 0) return {};

	const std::uint32_t target{packedTarget(header)};
	const bool has_block_target{(target & 0x003fffffU) != 0};

	const auto verus_version{(static_cast<std::uint32_t>(header[68]) << 24) |
							 (static_cast<std::uint32_t>(header[69]) << 16) |
							 (static_cast<std::uint32_t>(header[70]) << 8) |
							 static_cast<std::uint32_t>(header[71])};
	const auto verus_hash{Verus::VerusHasher{}
							  .write(std::span<const std::uint8_t>{header.data(), header.size()})
							  .finalize(verus_version != 2)};
	const CustomFloat verus_value{verus_hash};
	constexpr CustomFloat exponent{0, 3006477107ULL};
	const CustomFloat janus_value{verus_value * pow(sha_value, exponent)};
	return {true, has_block_target && belowTarget(janus_value, target),
			belowDifficulty(janus_value, share_difficulty)};
}

}  // namespace brminer::warthog
