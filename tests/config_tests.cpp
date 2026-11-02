#include "../src/config.hpp"
#include "../src/miner_options.hpp"


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

    const auto mining = parseWarthogOptions({"-a", "warthog", "-p", "stratum+tcp://node.example.net:3456",
        "-w", "testwallet", "--cpu_threads", "14"});
    assert(mining && mining->pool == "node.example.net:3456");
    assert(mining->wallet == "testwallet" && mining->threads == "14");
    assert(!parseWarthogOptions({"-regtest"}));
    const auto inherited = parseWarthogOptions({"-a", "wart", "-w", "newwallet"},
        {"oldwallet", "4", "pool.example.net:3033"});
    assert(inherited->wallet == "newwallet" && inherited->threads == "4");
    const auto tuning = parseWarthogOptions({"-a", "warthog", "--retune", "--cuda-batch-size", "16777216",
        "--cuda-block-size", "128", "--profile-dir", "profiles"});
    assert(tuning->tuning.autotune && tuning->tuning.retune);
    assert(*tuning->tuning.cuda_batch_size == 16777216 && *tuning->tuning.cuda_block_size == 128);
    assert(tuning->tuning.profile_directory == "profiles");
    for (const auto& args : std::vector<std::vector<std::string_view>>{
            {"-a", "warthog", "-w"}, {"-a", "warthog", "--cpu_threads", "0"},
            {"-a", "unknown"}, {"-a", "warthog", "--typo", "1"},
            {"-a", "warthog", "-p", "stratum+ssl://host:443"}}) {
        bool invalid{};
        try { parseWarthogOptions(args); } catch (const std::invalid_argument&) { invalid = true; }
        assert(invalid);
    }

    rejected = false;
    try {
        validateMiningSettings("", "2");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
