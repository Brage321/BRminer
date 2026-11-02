#include "brminer_verus_opt.hpp"
#include "crypto/verushash/verus_clhash_port.hpp"
#include "crypto/verushash/verushash.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>

namespace {
using Key = std::array<std::uint8_t, 2 * Verus::keySizeInBytes>;

void fillKey(Key& key, const std::array<std::uint8_t, 64>& input) {
    const unsigned char* previous = input.data();
    for (std::size_t i = 0; i < Verus::key256blocks; ++i) {
        haraka256_port(key.data() + i * 32, previous);
        previous = key.data() + i * 32;
    }
    std::memcpy(key.data() + Verus::keySizeInBytes, key.data(), Verus::keyRefreshsize);
}

void compareClhash(const std::array<std::uint8_t, 64>& input, bool version_2_2) {
    alignas(32) Key portable{};
    alignas(32) Key optimized{};
    fillKey(portable, input);
    optimized = portable;
    auto* portable_scratch = reinterpret_cast<__m128i**>(
        portable.data() + Verus::keySizeInBytes + Verus::keyRefreshsize);
    auto* optimized_scratch = reinterpret_cast<__m128i**>(
        optimized.data() + Verus::keySizeInBytes + Verus::keyRefreshsize);
    const auto portable_hash = version_2_2
        ? verusclhash_sv2_2_port(portable.data(), input.data(), Verus::keyMask, portable_scratch)
        : verusclhash_sv2_1_port(portable.data(), input.data(), Verus::keyMask, portable_scratch);
    const auto optimized_hash = version_2_2
        ? verusclhash_sv2_2(optimized.data(), input.data(), Verus::keyMask, optimized_scratch)
        : verusclhash_sv2_1(optimized.data(), input.data(), Verus::keyMask, optimized_scratch);
    assert(portable_hash == optimized_hash);
    assert(std::memcmp(portable.data(), optimized.data(), Verus::keySizeInBytes) == 0);
}

Hash portableHash(const std::array<std::uint8_t, 80>& header, bool version_2_2) {
    alignas(32) std::array<std::uint8_t, 64> first{};
    alignas(32) std::array<std::uint8_t, 64> second{};
    std::memcpy(first.data() + 32, header.data(), 32);
    haraka512_port(second.data(), first.data());
    std::memcpy(second.data() + 32, header.data() + 32, 32);
    haraka512_port(first.data(), second.data());
    std::memcpy(first.data() + 32, header.data() + 64, 16);
    std::memcpy(first.data() + 48, first.data(), 16);
    if (version_2_2) compareClhash(first, true);

    alignas(32) Key key{};
    fillKey(key, first);
    auto* scratch = reinterpret_cast<__m128i**>(
        key.data() + Verus::keySizeInBytes + Verus::keyRefreshsize);
    const std::uint64_t intermediate = version_2_2
        ? verusclhash_sv2_2_port(key.data(), first.data(), Verus::keyMask, scratch)
        : verusclhash_sv2_1_port(key.data(), first.data(), Verus::keyMask, scratch);
    std::memcpy(first.data() + 48, &intermediate, sizeof(intermediate));
    std::memcpy(first.data() + 56, &intermediate, sizeof(intermediate));
    Hash hash;
    haraka512_port_keyed(hash.data(), first.data(),
        reinterpret_cast<const u128*>(key.data()) + (intermediate & Verus::keyMask16));
    return hash;
}
}  // namespace

int main() {
    const bool simd_available{Verus::can_optimize()};
    for (std::uint32_t seed = 0; simd_available && seed < 8; ++seed) {
        alignas(32) std::array<std::uint8_t, 64> input{};
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::uint8_t>((i * 73 + seed * 41) ^ (i >> 1));
        }
        std::array<std::uint8_t, 32> portable{};
        std::array<std::uint8_t, 32> optimized{};
        haraka256_port(portable.data(), input.data());
        haraka256(optimized.data(), input.data());
        assert(portable == optimized);
        haraka512_port(portable.data(), input.data());
        haraka512(optimized.data(), input.data());
        assert(portable == optimized);
        compareClhash(input, false);
        compareClhash(input, true);
    }

    // Reusing a header prefix must agree with a fresh portable hash, even when
    // alternating jobs or Verus protocol versions on the same worker thread.
    Verus::VerusHasher reused;
    for (std::uint32_t sequence = 0; sequence < 256; ++sequence) {
        std::array<std::uint8_t, 80> header{};
        for (std::size_t i = 0; i < header.size(); ++i) {
            header[i] = static_cast<std::uint8_t>((i * 17 + (sequence / 32) * 53) ^ (i >> 2));
        }
        const std::uint32_t nonce = sequence * 0x01020304U;
        std::memcpy(header.data() + 76, &nonce, sizeof(nonce));
        const bool version_2_2 = (sequence & 1U) != 0;
        const auto expected = portableHash(header, version_2_2);
        const auto actual = Verus::VerusHasher{}
            .writeHeader80(header.data())
            .finalize(version_2_2);
        assert(actual == expected);
        const auto uncached = Verus::VerusHasher{}
            .write(std::span<const std::uint8_t>{header.data(), header.size()})
            .finalize(version_2_2);
        assert(actual == uncached);
        assert(reused.writeHeader80(header.data()).finalize(version_2_2) == expected);
    }
}
