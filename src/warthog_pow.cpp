#include "warthog_pow.hpp"

#include <crypto/sha256.h>
#include <block/header/custom_float.hpp>
#include <block/header/difficulty.hpp>
#include <crypto/verushash/verushash.hpp>
#if defined(BRMINER_NATIVE_BATCH)
#include "warthog_native_batch/warthog_native_batch.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

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
	thread_local double cached_difficulty{};
	thread_local TargetV2 target{1.0};
	if (cached_difficulty != difficulty) {
		target = TargetV2{difficulty};
		cached_difficulty = difficulty;
	}
	const std::uint32_t target_zeros{target.zeros10()};
	const std::uint32_t hash_zeros{static_cast<std::uint32_t>(-hash.exponent())};
	if (target_zeros > hash_zeros) return false;
	if (target_zeros < hash_zeros) return true;
	return hash.mantissa() < (static_cast<std::uint64_t>(target.bits22()) << 10);
}

bool verusPrefixMayMeetTarget(std::uint32_t prefix, std::uint32_t target_zeros) {
	// Every eligible SHA value is at least 0.005, so SHA^0.7 exceeds 2^-10.
	// A Janus proof therefore requires Verus < 2^(10-target_zeros).
	// The target mantissa is strictly below one, making this prefix bound safe.
	if (target_zeros <= 10) return true;
	if (target_zeros >= 42) return prefix == 0;
	return prefix < (std::uint32_t{1} << (42 - target_zeros));
}

PowResult scoreJanusHashValue(const Header& header, const CustomFloat& sha_value,
						 const Hash& verus_hash, double share_difficulty) {
	const std::uint32_t target{packedTarget(header)};
	const bool has_block_target{(target & 0x003fffffU) != 0};
	const CustomFloat verus_value{verus_hash};
	// SHA candidates are at least 0.005; their 0.7 power exceeds 1/1024.
	// This bound can only reject hashes that miss both Janus targets.
	constexpr CustomFloat conservative_sha_power{-9, 2147483648ULL};
	const CustomFloat minimum_janus{verus_value * conservative_sha_power};
	if ((!has_block_target || !belowTarget(minimum_janus, target)) &&
		!belowDifficulty(minimum_janus, share_difficulty)) {
		return {true, false, false};
	}
	constexpr CustomFloat exponent{0, 3006477107ULL};
	const CustomFloat janus_value{verus_value * pow(sha_value, exponent)};
	return {true, has_block_target && belowTarget(janus_value, target),
			belowDifficulty(janus_value, share_difficulty)};
}

PowResult checkJanusHashValue(const Header& header, const CustomFloat& sha_value,
						 double share_difficulty) {
	constexpr CustomFloat lower_bound{-7, 2748779069ULL};
	if (comparePositive(sha_value, lower_bound) < 0) return {};
	const auto verus_version{(static_cast<std::uint32_t>(header[68]) << 24) |
							 (static_cast<std::uint32_t>(header[69]) << 16) |
							 (static_cast<std::uint32_t>(header[70]) << 8) |
							 static_cast<std::uint32_t>(header[71])};
	thread_local Verus::VerusHasher hasher;
	const auto verus_hash{hasher.writeHeader80(header.data()).finalize(verus_version != 2)};
	return scoreJanusHashValue(header, sha_value, verus_hash, share_difficulty);
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
	return checkJanusHashValue(header, sha_value, share_difficulty);
}

PowResult checkJanusHashFromShaWord(const Header& header, std::uint32_t sha_word,
									 double share_difficulty) {
	if (sha_word == 0) return {};
	const auto shift{std::countl_zero(sha_word)};
	return checkJanusHashValue(header,
		CustomFloat{-shift, static_cast<std::uint64_t>(sha_word) << shift},
		share_difficulty);
}

std::array<PowResult, 2> checkJanusHashFromShaWordPair(
	const Header& first, std::uint32_t first_sha_word,
	const Header& second, std::uint32_t second_sha_word,
	double share_difficulty) {
#if defined(BRMINER_NATIVE_BATCH)
	static const bool native_supported{brminer_native_cpu_supported() != 0};
	const auto version{(static_cast<std::uint32_t>(first[68]) << 24) |
		(static_cast<std::uint32_t>(first[69]) << 16) |
		(static_cast<std::uint32_t>(first[70]) << 8) | first[71]};
	const auto eligible_word = [](std::uint32_t word) {
		if (word == 0) return false;
		const auto shift{std::countl_zero(word)};
		constexpr CustomFloat lower_bound{-7, 2748779069ULL};
		return comparePositive(CustomFloat{-shift, static_cast<std::uint64_t>(word) << shift},
			lower_bound) >= 0;
	};
	if (native_supported && version != 2 &&
		eligible_word(first_sha_word) && eligible_word(second_sha_word) &&
		std::memcmp(first.data(), second.data(), 76) == 0) {
		const auto nonce_word = [](const Header& header) {
			return (static_cast<std::uint32_t>(header[76]) << 24) |
				(static_cast<std::uint32_t>(header[77]) << 16) |
				(static_cast<std::uint32_t>(header[78]) << 8) | header[79];
		};
		const std::array<std::uint32_t, 2> nonces{nonce_word(first), nonce_word(second)};
		std::array<std::uint8_t, 64> hashes{};
		if (brminer_native_batch_hash(first.data(), nonces.data(), nonces.size(), hashes.data()) != 0) {
			throw std::runtime_error("Paired native Verus hash failed.");
		}
		Hash first_verus, second_verus;
		std::memcpy(first_verus.data(), hashes.data(), first_verus.size());
		std::memcpy(second_verus.data(), hashes.data() + 32, second_verus.size());
		const auto sha_value = [](std::uint32_t word) {
			const auto shift{std::countl_zero(word)};
			return CustomFloat{-shift, static_cast<std::uint64_t>(word) << shift};
		};
		return {scoreJanusHashValue(first, sha_value(first_sha_word), first_verus, share_difficulty),
			scoreJanusHashValue(second, sha_value(second_sha_word), second_verus, share_difficulty)};
	}
#endif
	return {checkJanusHashFromShaWord(first, first_sha_word, share_difficulty),
		checkJanusHashFromShaWord(second, second_sha_word, share_difficulty)};
}

void checkJanusHashFromShaWordBatch(const Header& job_header,
	std::span<const Sha256tCandidate> candidates, double share_difficulty,
	std::vector<PowResult>& results) {
	results.resize(candidates.size());
	if (candidates.empty()) return;
#if defined(BRMINER_NATIVE_BATCH)
	static const bool native_supported{brminer_native_cpu_supported() != 0};
	const auto version{(static_cast<std::uint32_t>(job_header[68]) << 24) |
		(static_cast<std::uint32_t>(job_header[69]) << 16) |
		(static_cast<std::uint32_t>(job_header[70]) << 8) | job_header[71]};
	if (native_supported && version != 2) {
		const std::uint32_t block_target{packedTarget(job_header)};
		const bool has_block_target{(block_target & 0x003fffffU) != 0};
		const bool has_share_target{std::isfinite(share_difficulty) &&
			share_difficulty > 0.0};
		const std::uint32_t share_target_zeros{has_share_target
			? TargetV2{share_difficulty}.zeros10() : 0};
		thread_local std::vector<std::uint32_t> nonces;
		thread_local std::vector<std::uint8_t> hashes;
		nonces.resize(candidates.size());
		hashes.resize(candidates.size() * 32);
		for (std::size_t i{}; i < candidates.size(); ++i) {
			const auto nonce{candidates[i].nonce};
			nonces[i] = (nonce << 24) | ((nonce & 0x0000ff00U) << 8) |
				((nonce & 0x00ff0000U) >> 8) | (nonce >> 24);
		}
		if (brminer_native_batch_hash(job_header.data(), nonces.data(), nonces.size(),
			hashes.data()) != 0) {
			throw std::runtime_error("Native Verus candidate batch failed.");
		}
		for (std::size_t i{}; i < candidates.size(); ++i) {
			const auto word{candidates[i].sha_word};
			// 0.005 lies strictly between these adjacent 32-bit SHA words.
			if (word <= 0x0147ae14U) {
				results[i] = {};
				continue;
			}
			const auto* hash_bytes{hashes.data() + i * 32};
			const std::uint32_t verus_prefix{
				(static_cast<std::uint32_t>(hash_bytes[0]) << 24) |
				(static_cast<std::uint32_t>(hash_bytes[1]) << 16) |
				(static_cast<std::uint32_t>(hash_bytes[2]) << 8) | hash_bytes[3]};
			if ((!has_block_target ||
				 !verusPrefixMayMeetTarget(verus_prefix, block_target >> 22)) &&
				(!has_share_target ||
				 !verusPrefixMayMeetTarget(verus_prefix, share_target_zeros))) {
				results[i] = {true, false, false};
				continue;
			}
			Header header{job_header};
			const auto nonce{candidates[i].nonce};
			header[76] = static_cast<std::uint8_t>(nonce);
			header[77] = static_cast<std::uint8_t>(nonce >> 8);
			header[78] = static_cast<std::uint8_t>(nonce >> 16);
			header[79] = static_cast<std::uint8_t>(nonce >> 24);
			const auto shift{std::countl_zero(word)};
			const CustomFloat sha_value{-shift, static_cast<std::uint64_t>(word) << shift};
			Hash verus_hash;
			std::memcpy(verus_hash.data(), hashes.data() + i * 32, 32);
			results[i] = scoreJanusHashValue(header, sha_value,
				verus_hash, share_difficulty);
		}
		return;
	}
#endif
	for (std::size_t i{}; i < candidates.size(); ++i) {
		Header header{job_header};
		const auto nonce{candidates[i].nonce};
		header[76] = static_cast<std::uint8_t>(nonce);
		header[77] = static_cast<std::uint8_t>(nonce >> 8);
		header[78] = static_cast<std::uint8_t>(nonce >> 16);
		header[79] = static_cast<std::uint8_t>(nonce >> 24);
		results[i] = checkJanusHashFromShaWord(header, candidates[i].sha_word,
			share_difficulty);
	}
}

}  // namespace brminer::warthog
