#pragma once

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace brminer {

struct PoolEndpoint {
    std::string host;
    std::string port;
};

inline PoolEndpoint parsePoolEndpoint(std::string_view text) {
    if (text.empty()) text = "mine.bobfarm.icu:3033";
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == text.size() ||
        text.find(':', separator + 1) != std::string_view::npos) {
        throw std::invalid_argument("NODE must be a pool hostname and port, for example mine.bobfarm.icu:3033.");
    }

    const std::string_view host = text.substr(0, separator);
    for (const unsigned char character : host) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' || character == '-')) {
            throw std::invalid_argument("NODE hostname may contain only letters, numbers, dots, and hyphens.");
        }
    }

    unsigned int numeric_port = 0;
    const std::string_view port = text.substr(separator + 1);
    const auto result = std::from_chars(port.data(), port.data() + port.size(), numeric_port);
    if (result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
        numeric_port < 1 || numeric_port > 65535) {
        throw std::invalid_argument("NODE port must be a number from 1 to 65535.");
    }
    return {std::string{host}, std::string{port}};
}

inline int parseThreadCount(std::string_view text) {
    int threads = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), threads);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        threads < 1 || threads > 1024) {
        throw std::invalid_argument("THREADS must be a whole number from 1 to 1024.");
    }
    return threads;
}

inline void validateMiningSettings(std::string_view payoutAddress, std::string_view threads) {
    if (payoutAddress.empty()) {
        throw std::invalid_argument("Set PAYOUT_ADDRESS in mining-connectcoin.bat.");
    }
    (void)parseThreadCount(threads);
}

}  // namespace brminer
