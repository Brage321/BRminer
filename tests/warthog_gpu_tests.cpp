#include "../src/warthog_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct ExpectedCandidate {
	std::uint32_t nonce{};
	std::array<std::uint8_t, 32> hash{};
};

bool matchesCompactCandidate(const brminer::warthog::Sha256tCandidate& gpu,
							 const ExpectedCandidate& cpu) {
	const std::uint32_t first_word{(static_cast<std::uint32_t>(cpu.hash[0]) << 24) |
		(static_cast<std::uint32_t>(cpu.hash[1]) << 16) |
		(static_cast<std::uint32_t>(cpu.hash[2]) << 8) | cpu.hash[3]};
	return gpu.nonce == cpu.nonce && gpu.sha_word == first_word;
}
}  // namespace

int main(int argc, char** argv) {
	using namespace brminer::warthog;
	const auto devices{OpenClSha256tMiner::devices()};
	if (devices.empty()) {
		std::cout << "No OpenCL GPU detected; GPU kernel execution was not available for this test run.\n";
		return 0;
	}

	constexpr std::uint32_t nonce_count{256};
	Header header{};
	OpenClSha256tMiner miner{0};
	auto candidates{miner.scan(header, 0, nonce_count)};
	std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
		return left.nonce < right.nonce;
	});
	std::vector<ExpectedCandidate> expected;
	for (std::uint32_t nonce{}; nonce < nonce_count; ++nonce) {
		Header candidate_header{header};
		candidate_header[76] = static_cast<std::uint8_t>(nonce);
		candidate_header[77] = static_cast<std::uint8_t>(nonce >> 8);
		candidate_header[78] = static_cast<std::uint8_t>(nonce >> 16);
		candidate_header[79] = static_cast<std::uint8_t>(nonce >> 24);
		const auto hash{sha256t(candidate_header)};
		const std::uint32_t first_word{(static_cast<std::uint32_t>(hash[0]) << 24) |
			(static_cast<std::uint32_t>(hash[1]) << 16) |
			(static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
		if (first_word >= 0x0147ae14U) expected.push_back({nonce, hash});
	}
	if (candidates.size() != expected.size()) {
		throw std::runtime_error("OpenCL SHA256t kernel returned a different candidate count than the CPU.");
	}
	for (std::size_t i{}; i < candidates.size(); ++i) {
		if (!matchesCompactCandidate(candidates[i], expected[i])) {
			throw std::runtime_error("OpenCL SHA256t candidate differs from the CPU consensus implementation.");
		}
		Header candidate_header{header};
		candidate_header[76] = static_cast<std::uint8_t>(candidates[i].nonce);
		candidate_header[77] = static_cast<std::uint8_t>(candidates[i].nonce >> 8);
		candidate_header[78] = static_cast<std::uint8_t>(candidates[i].nonce >> 16);
		candidate_header[79] = static_cast<std::uint8_t>(candidates[i].nonce >> 24);
		std::array<std::uint8_t, 32> compact_hash{};
		compact_hash[0] = static_cast<std::uint8_t>(candidates[i].sha_word >> 24);
		compact_hash[1] = static_cast<std::uint8_t>(candidates[i].sha_word >> 16);
		compact_hash[2] = static_cast<std::uint8_t>(candidates[i].sha_word >> 8);
		compact_hash[3] = static_cast<std::uint8_t>(candidates[i].sha_word);
		const auto compact_result{checkJanusHash(candidate_header, compact_hash, 1.0)};
		const auto word_result{checkJanusHashFromShaWord(candidate_header, candidates[i].sha_word, 1.0)};
		const auto full_result{checkJanusHash(candidate_header, expected[i].hash, 1.0)};
		if (word_result.sha256t_candidate != compact_result.sha256t_candidate ||
			word_result.meets_target != compact_result.meets_target ||
			word_result.meets_share_target != compact_result.meets_share_target ||
			compact_result.sha256t_candidate != full_result.sha256t_candidate ||
			compact_result.meets_target != full_result.meets_target ||
			compact_result.meets_share_target != full_result.meets_share_target) {
			throw std::runtime_error("Compact GPU candidate changes the Janus target result.");
		}
	}

	constexpr std::uint32_t candidate_threshold{0x80000000U};
	auto filtered_candidates{miner.scan(header, 0, nonce_count, candidate_threshold)};
	std::sort(filtered_candidates.begin(), filtered_candidates.end(), [](const auto& left, const auto& right) {
		return left.nonce < right.nonce;
	});
	std::erase_if(expected, [](const auto& candidate) {
		const std::uint32_t first_word{(static_cast<std::uint32_t>(candidate.hash[0]) << 24) |
			(static_cast<std::uint32_t>(candidate.hash[1]) << 16) |
			(static_cast<std::uint32_t>(candidate.hash[2]) << 8) | candidate.hash[3]};
		return first_word >= candidate_threshold;
	});
	if (filtered_candidates.size() != expected.size()) {
		throw std::runtime_error("OpenCL SHA256t candidate threshold returned a different candidate count than the CPU.");
	}
	for (std::size_t i{}; i < filtered_candidates.size(); ++i) {
		if (!matchesCompactCandidate(filtered_candidates[i], expected[i])) {
			throw std::runtime_error("OpenCL SHA256t candidate threshold differs from the CPU.");
		}
	}
	Header changed_header{header};
	changed_header[0] = 1;
	auto changed_candidates{miner.scan(changed_header, 0, nonce_count)};
	std::sort(changed_candidates.begin(), changed_candidates.end(), [](const auto& left, const auto& right) {
		return left.nonce < right.nonce;
	});
	std::vector<ExpectedCandidate> changed_expected;
	for (std::uint32_t nonce{}; nonce < nonce_count; ++nonce) {
		Header candidate_header{changed_header};
		candidate_header[76] = static_cast<std::uint8_t>(nonce);
		candidate_header[77] = static_cast<std::uint8_t>(nonce >> 8);
		candidate_header[78] = static_cast<std::uint8_t>(nonce >> 16);
		candidate_header[79] = static_cast<std::uint8_t>(nonce >> 24);
		const auto hash{sha256t(candidate_header)};
		const std::uint32_t first_word{(static_cast<std::uint32_t>(hash[0]) << 24) |
			(static_cast<std::uint32_t>(hash[1]) << 16) |
			(static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
		if (first_word >= 0x0147ae14U) changed_expected.push_back({nonce, hash});
	}
	if (changed_candidates.size() != changed_expected.size()) {
		throw std::runtime_error("OpenCL header update returned a different candidate count than the CPU.");
	}
	for (std::size_t i{}; i < changed_candidates.size(); ++i) {
		if (!matchesCompactCandidate(changed_candidates[i], changed_expected[i])) {
			throw std::runtime_error("OpenCL header update candidate differs from the CPU.");
		}
	}

	constexpr std::uint32_t large_nonce_start{0x12340000U};
	constexpr std::uint32_t large_nonce_count{4194304};
	constexpr std::uint32_t narrow_threshold{0x0149ae14U};
	auto large_candidates{miner.scan(header, large_nonce_start, large_nonce_count, narrow_threshold)};
	std::sort(large_candidates.begin(), large_candidates.end(), [](const auto& left, const auto& right) {
		return left.nonce < right.nonce;
	});
	std::vector<ExpectedCandidate> large_expected;
	for (std::uint32_t offset{}; offset < large_nonce_count; ++offset) {
		const std::uint32_t nonce{large_nonce_start + offset};
		Header candidate_header{header};
		candidate_header[76] = static_cast<std::uint8_t>(nonce);
		candidate_header[77] = static_cast<std::uint8_t>(nonce >> 8);
		candidate_header[78] = static_cast<std::uint8_t>(nonce >> 16);
		candidate_header[79] = static_cast<std::uint8_t>(nonce >> 24);
		const auto hash{sha256t(candidate_header)};
		const std::uint32_t first_word{(static_cast<std::uint32_t>(hash[0]) << 24) |
			(static_cast<std::uint32_t>(hash[1]) << 16) |
			(static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
		if (first_word >= 0x0147ae14U && first_word < narrow_threshold) {
			large_expected.push_back({nonce, hash});
		}
	}
	if (large_candidates.size() != large_expected.size()) {
		throw std::runtime_error("Large OpenCL batch returned a different candidate count than the CPU.");
	}
	for (std::size_t i{}; i < large_candidates.size(); ++i) {
		if (!matchesCompactCandidate(large_candidates[i], large_expected[i])) {
			throw std::runtime_error("Large OpenCL batch candidate differs from the CPU.");
		}
	}
	if (argc == 2 && std::string_view{argv[1]} == "--bench") {
		constexpr std::uint32_t count{4194304};
		constexpr int batches{480};
		std::size_t found{};
		const auto begin{std::chrono::steady_clock::now()};
		for (int i{}; i < batches; ++i)
			found += miner.scan(header, static_cast<std::uint32_t>(i) * count,
				count, 0x08000000U).size();
		const double seconds{std::chrono::duration<double>(
			std::chrono::steady_clock::now() - begin).count()};
		std::cout << "OpenCL scan MH/s=" <<
			(static_cast<double>(count) * batches / seconds / 1e6)
			<< " candidates=" << found << " seconds=" << seconds << ".\n";
	}
	std::cout << "OpenCL SHA256t kernel passed on " << miner.deviceName() << ".\n";
}
