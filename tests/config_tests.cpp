#include "../src/config.hpp"


#include <cassert>
#include <stdexcept>

int main() {
    using namespace brminer;

    assert(parseThreadCount("1") == 1);
    assert(parseThreadCount("1024") == 1024);
    const PoolEndpoint defaultPool = parsePoolEndpoint("");
    assert(defaultPool.host == "mine.bobfarm.icu");
    assert(defaultPool.port == "3033");
    const PoolEndpoint customPool = parsePoolEndpoint("pool.example.net:1234");
    assert(customPool.host == "pool.example.net");
    assert(customPool.port == "1234");
    bool invalidNode = false;
    try {
        parsePoolEndpoint("pool.example.net:70000");
    } catch (const std::invalid_argument&) {
        invalidNode = true;
    }
    assert(invalidNode);
    validateMiningSettings("connectcoin-address", "2");

    bool rejected = false;
    try {
        parseThreadCount("0");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        validateMiningSettings("", "2");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
