#include "warthog_pow.hpp"

#include <block/header/custom_float.hpp>
#include <block/header/difficulty.hpp>
#include <crypto/verushash/verushash.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace {
using namespace brminer::warthog;

bool belowEncoded(const CustomFloat& value, std::uint32_t encoded) {
    const std::uint32_t mantissa{encoded & 0x003fffffU};
    if (mantissa == 0) return false;
    const std::uint32_t zeros{encoded >> 22};
    const std::uint32_t hash_zeros{static_cast<std::uint32_t>(-value.exponent())};
    return zeros < hash_zeros ||
        (zeros == hash_zeros && value.mantissa() < (static_cast<std::uint64_t>(mantissa) << 10));
}

PowResult referenceCheck(const Header& header, const std::array<std::uint8_t, 32>& sha_hash,
                         double difficulty) {
    const CustomFloat sha{sha_hash};
    constexpr CustomFloat floor{-7, 2748779069ULL};
    if (sha.exponent() < floor.exponent() ||
        (sha.exponent() == floor.exponent() && sha.mantissa() < floor.mantissa())) return {};
    const std::uint32_t encoded{(static_cast<std::uint32_t>(header[32]) << 24) |
        (static_cast<std::uint32_t>(header[33]) << 16) |
        (static_cast<std::uint32_t>(header[34]) << 8) | header[35]};
    const std::uint32_t version{(static_cast<std::uint32_t>(header[68]) << 24) |
        (static_cast<std::uint32_t>(header[69]) << 16) |
        (static_cast<std::uint32_t>(header[70]) << 8) | header[71]};
    const auto hash{Verus::VerusHasher{}
        .write(std::span<const std::uint8_t>{header.data(), header.size()})
        .finalize(version != 2)};
    constexpr CustomFloat exponent{0, 3006477107ULL};
    const CustomFloat janus{CustomFloat{hash} * pow(sha, exponent)};
    bool share{};
    if (std::isfinite(difficulty) && difficulty > 0.0) {
        const TargetV2 target{difficulty};
        const std::uint32_t zeros{target.zeros10()};
        const std::uint32_t hash_zeros{static_cast<std::uint32_t>(-janus.exponent())};
        share = zeros < hash_zeros ||
            (zeros == hash_zeros &&
             janus.mantissa() < (static_cast<std::uint64_t>(target.bits22()) << 10));
    }
    return {true, belowEncoded(janus, encoded), share};
}
}  // namespace

int main() {
    for (std::uint32_t sequence{}; sequence < 20; ++sequence) {
        Header candidate{};
        for (std::size_t i{}; i < candidate.size(); ++i) {
            candidate[i] = static_cast<std::uint8_t>(i * 29 + sequence * 13);
        }
        candidate[68] = candidate[69] = candidate[70] = 0;
        candidate[71] = (sequence & 1U) ? 2 : 3;
        for (const std::uint8_t first : {std::uint8_t{1}, std::uint8_t{3},
                                          std::uint8_t{8}, std::uint8_t{64},
                                          std::uint8_t{128}}) {
            std::array<std::uint8_t, 32> fake_sha{};
            for (std::size_t i{}; i < fake_sha.size(); ++i) {
                fake_sha[i] = static_cast<std::uint8_t>(i * 17 + sequence);
            }
            fake_sha[0] = first;
            std::array<std::uint8_t, 32> compact_sha{};
            for (std::size_t i{}; i < 4; ++i) compact_sha[i] = fake_sha[i];
            const std::uint32_t sha_word{(static_cast<std::uint32_t>(fake_sha[0]) << 24) |
                (static_cast<std::uint32_t>(fake_sha[1]) << 16) |
                (static_cast<std::uint32_t>(fake_sha[2]) << 8) | fake_sha[3]};
            for (const double difficulty : {1.0e-20, 1.0, 1.0e4, 1.0e12, 1.0e20}) {
                const auto expected{referenceCheck(candidate, fake_sha, difficulty)};
                const auto actual{checkJanusHash(candidate, fake_sha, difficulty)};
                assert(actual.sha256t_candidate == expected.sha256t_candidate);
                assert(actual.meets_target == expected.meets_target);
                assert(actual.meets_share_target == expected.meets_share_target);
                const auto compact_expected{checkJanusHash(candidate, compact_sha, difficulty)};
                const auto word_actual{checkJanusHashFromShaWord(candidate, sha_word, difficulty)};
                assert(word_actual.sha256t_candidate == compact_expected.sha256t_candidate);
                assert(word_actual.meets_target == compact_expected.meets_target);
                assert(word_actual.meets_share_target == compact_expected.meets_share_target);
            }
        }
		Header second{candidate};
		candidate[76] = static_cast<std::uint8_t>(sequence);
		candidate[77] = 0x31;
		second[76] = static_cast<std::uint8_t>(sequence + 17);
		second[77] = 0x82;
		const std::uint32_t first_word{0x2a112233U + sequence};
		const std::uint32_t second_word{0x7199aabbU - sequence};
		for (const double difficulty : {1.0e-20, 1.0, 1.0e4, 1.0e12}) {
			const auto paired{checkJanusHashFromShaWordPair(candidate, first_word,
				second, second_word, difficulty)};
			const auto first_single{checkJanusHashFromShaWord(candidate, first_word, difficulty)};
			const auto second_single{checkJanusHashFromShaWord(second, second_word, difficulty)};
			assert(paired[0].sha256t_candidate == first_single.sha256t_candidate);
			assert(paired[0].meets_target == first_single.meets_target);
			assert(paired[0].meets_share_target == first_single.meets_share_target);
			assert(paired[1].sha256t_candidate == second_single.sha256t_candidate);
			assert(paired[1].meets_target == second_single.meets_target);
			assert(paired[1].meets_share_target == second_single.meets_share_target);
			const auto low_pair{checkJanusHashFromShaWordPair(candidate, 1U,
				second, second_word, difficulty)};
			assert(!low_pair[0].sha256t_candidate);
			assert(low_pair[1].sha256t_candidate == second_single.sha256t_candidate);
		}
		const std::array<Sha256tCandidate, 7> batch{{
			{0, 0x2a112233U}, {0x01020304U, 0x7199aabbU},
			{UINT32_MAX, 1U}, {0x01020304U, 0x0147ae14U},
			{sequence, 0x92345678U}, {sequence + 1, 0x0147ae15U},
			{sequence + 2, 0x0147ae16U}}};
		const std::array<std::uint32_t, 4> encoded_targets{
			0x00300000U, 0x0a300000U, 0x0b300000U, 0x7f300000U};
		const auto encoded{encoded_targets[sequence % encoded_targets.size()]};
		candidate[32] = static_cast<std::uint8_t>(encoded >> 24);
		candidate[33] = static_cast<std::uint8_t>(encoded >> 16);
		candidate[34] = static_cast<std::uint8_t>(encoded >> 8);
		candidate[35] = static_cast<std::uint8_t>(encoded);
		for (const double difficulty : {0.0, 1.0, 1.0e4, 1.0e12, 1.0e20}) {
			std::vector<PowResult> batch_results;
			checkJanusHashFromShaWordBatch(candidate, batch, difficulty, batch_results);
			assert(batch_results.size() == batch.size());
			for (std::size_t i{}; i < batch.size(); ++i) {
				Header item{candidate};
				item[76] = static_cast<std::uint8_t>(batch[i].nonce);
				item[77] = static_cast<std::uint8_t>(batch[i].nonce >> 8);
				item[78] = static_cast<std::uint8_t>(batch[i].nonce >> 16);
				item[79] = static_cast<std::uint8_t>(batch[i].nonce >> 24);
				const auto expected{checkJanusHashFromShaWord(item, batch[i].sha_word,
					difficulty)};
				assert(batch_results[i].sha256t_candidate == expected.sha256t_candidate);
				assert(batch_results[i].meets_target == expected.meets_target);
				assert(batch_results[i].meets_share_target == expected.meets_share_target);
			}
		}
    }
}
