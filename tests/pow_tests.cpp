#include "../src/pow.hpp"
#include "../src/randomx_hasher.hpp"

#include <cassert>
#include <stdexcept>
#include <string_view>

int main() {
    using namespace brminer;

    const auto powLimit =
        parseDisplayedHash("0000ffff00000000000000000000000000000000000000000000000000000000");
    const auto maxTarget = decodeCompactTarget(0x1f00ffff, powLimit);
    assert(maxTarget.has_value());
    assert(*maxTarget == powLimit);

    const auto initialTarget = decodeCompactTarget(0x1e333300, powLimit);
    assert(initialTarget.has_value());
    assert(compareLittleEndian256(*initialTarget, powLimit) < 0);
    assert(!decodeCompactTarget(0x1d80ffff, powLimit).has_value());
    assert(!decodeCompactTarget(0, powLimit).has_value());
    assert(!decodeCompactTarget(0x2100ffff, powLimit).has_value());

    Hash256 equalHash = *initialTarget;
    assert(meetsTarget(equalHash, *initialTarget));
    equalHash[0] = static_cast<std::uint8_t>(equalHash[0] + 1);
    assert(!meetsTarget(equalHash, *initialTarget));

    BlockHeader header;
    header.setTimestamp(0x12345678);
    header.setBits(0x1e333300);
    header.setNonce(0x89abcdef);
    assert(header.bytes[68] == 0x78 && header.bytes[71] == 0x12);
    assert(header.bytes[72] == 0x00 && header.bytes[75] == 0x1e);
    assert(header.bytes[76] == 0xef && header.bytes[79] == 0x89);
    assert(header.nonce() == 0x89abcdef);

    assert(randomXSeedHeight(0) == 0);
    assert(randomXSeedHeight(64) == 0);
    assert(randomXSeedHeight(12'351) == 0);
    assert(randomXSeedHeight(12'352) == 12'288);
    assert(randomXSeedHeight(24'640) == 24'576);

    const auto bootstrapKey = mainnetBootstrapKey();
    assert(bootstrapKey.front() == 0xe5);
    assert(bootstrapKey.back() == 0xd9);

    bool invalidHexRejected = false;
    try {
        (void)parseDisplayedHash("bad");
    } catch (const std::invalid_argument&) {
        invalidHexRejected = true;
    }
    assert(invalidHexRejected);

    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    constexpr std::string_view expected{
        "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f"};
    const RandomXContext context(key);
    auto worker = context.createWorker();
    const Hash256 hash = worker.hash(input.data(), input.size());
    static constexpr char digits[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(hash.size() * 2);
    for (const std::uint8_t byte : hash) {
        actual.push_back(digits[byte >> 4]);
        actual.push_back(digits[byte & 0x0f]);
    }
    assert(actual == expected);
}
