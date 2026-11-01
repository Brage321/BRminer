#include "../src/warthog_gpu.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
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
	std::vector<Sha256tCandidate> expected;
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
		if (candidates[i].nonce != expected[i].nonce || candidates[i].hash != expected[i].hash) {
			throw std::runtime_error("OpenCL SHA256t candidate differs from the CPU consensus implementation.");
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
		if (filtered_candidates[i].nonce != expected[i].nonce || filtered_candidates[i].hash != expected[i].hash) {
			throw std::runtime_error("OpenCL SHA256t candidate threshold differs from the CPU.");
		}
	}
	std::cout << "OpenCL SHA256t kernel passed on " << miner.deviceName() << ".\n";
}
