#include "../src/warthog_pow.hpp"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
	using namespace brminer::warthog;
	static constexpr char digits[] = "0123456789abcdef";
	Header header{};
	const auto hash{sha256t(header)};
	std::string actual;
	actual.reserve(hash.size() * 2);
	for (const std::uint8_t byte : hash) {
		actual.push_back(digits[byte >> 4]);
		actual.push_back(digits[byte & 0x0f]);
	}
	assert(actual == "6f6fbf702166dbb4f25596c802d5170c2c62d0caf4f09e958cd929ba1e7bdf72");

	Header easy_target_header{header};
	easy_target_header[33] = 0x3f;
	easy_target_header[34] = 0xff;
	easy_target_header[35] = 0xff;
	const PowResult valid_target_result{checkJanusHash(easy_target_header)};
	assert(valid_target_result.sha256t_candidate);
	assert(valid_target_result.meets_target);
	const PowResult prehashed_target_result{checkJanusHash(easy_target_header, sha256t(easy_target_header))};
	assert(prehashed_target_result.sha256t_candidate == valid_target_result.sha256t_candidate);
	assert(prehashed_target_result.meets_target == valid_target_result.meets_target);

	const PowResult zero_target_result{checkJanusHash(header)};
	assert(zero_target_result.sha256t_candidate);
	assert(!zero_target_result.meets_target);

	const PowResult easy_share_result{checkJanusHash(header, 1.0e-20)};
	assert(easy_share_result.sha256t_candidate);
	assert(easy_share_result.meets_share_target);
	const PowResult impossible_share_result{checkJanusHash(header, 1.0e100)};
	assert(impossible_share_result.sha256t_candidate);
	assert(!impossible_share_result.meets_share_target);
}
