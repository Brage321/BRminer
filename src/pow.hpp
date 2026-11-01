#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace brminer {

using Hash256 = std::array<std::uint8_t, 32>;
using PowTarget = std::array<std::uint8_t, 32>;

struct BlockHeader {
    std::array<std::uint8_t, 80> bytes{};

    void setTimestamp(std::uint32_t timestamp) {
        writeUint32(68, timestamp);
    }

    void setBits(std::uint32_t bits) {
        writeUint32(72, bits);
    }

    void setNonce(std::uint32_t nonce) {
        writeUint32(76, nonce);
    }

    std::uint32_t nonce() const {
        return readUint32(76);
    }

private:
    void writeUint32(std::size_t offset, std::uint32_t value) {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
        }
    }

    std::uint32_t readUint32(std::size_t offset) const {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8);
        }
        return value;
    }
};

inline int compareLittleEndian256(const PowTarget& left, const PowTarget& right) {
    for (std::size_t i = left.size(); i > 0; --i) {
        if (left[i - 1] < right[i - 1]) return -1;
        if (left[i - 1] > right[i - 1]) return 1;
    }
    return 0;
}

inline std::optional<PowTarget> decodeCompactTarget(
    std::uint32_t compact,
    const PowTarget& powLimit
) {
    const std::uint32_t exponent = compact >> 24;
    const std::uint32_t mantissa = compact & 0x007fffff;
    const bool negative = (compact & 0x00800000) != 0 && mantissa != 0;
    const bool overflow = mantissa != 0 &&
        (exponent > 34 ||
         (mantissa > 0xff && exponent > 33) ||
         (mantissa > 0xffff && exponent > 32));
    if (negative || mantissa == 0 || overflow) {
        return std::nullopt;
    }

    PowTarget target{};
    if (exponent <= 3) {
        const std::uint32_t value = mantissa >> (8 * (3 - exponent));
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            target[i] = static_cast<std::uint8_t>(value >> (i * 8));
        }
    } else {
        const std::size_t offset = exponent - 3;
        for (std::size_t i = 0; i < 3; ++i) {
            if (offset + i < target.size()) {
                target[offset + i] = static_cast<std::uint8_t>(mantissa >> (i * 8));
            }
        }
    }

    if (compareLittleEndian256(target, powLimit) > 0) {
        return std::nullopt;
    }
    return target;
}

inline bool meetsTarget(const Hash256& hash, const PowTarget& target) {
    return compareLittleEndian256(hash, target) <= 0;
}

inline int randomXSeedHeight(int candidateHeight) {
    constexpr int epochBlocks = 12'288;
    constexpr int epochLag = 64;
    if (candidateHeight < 0) {
        throw std::invalid_argument("Block height cannot be negative.");
    }
    if (candidateHeight <= epochLag) {
        return 0;
    }
    return ((candidateHeight - epochLag) / epochBlocks) * epochBlocks;
}

inline Hash256 parseDisplayedHash(std::string_view hex) {
    if (hex.size() != 64) {
        throw std::invalid_argument("Hash must contain exactly 64 hexadecimal characters.");
    }
    Hash256 hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        const auto nibble = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
            if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
            if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
            throw std::invalid_argument("Hash must contain 64 hexadecimal characters.");
        };
        const std::size_t source = (hash.size() - 1 - i) * 2;
        hash[i] = static_cast<std::uint8_t>(
            (nibble(hex[source]) << 4) | nibble(hex[source + 1]));
    }
    return hash;
}

inline Hash256 mainnetBootstrapKey() {
    return parseDisplayedHash(
        "d91b262aecaac2c4868b2cbe1563538f107c33fbee8c5d373bdaa8e551567fe5");
}

}  // namespace brminer
