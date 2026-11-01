#include "config.hpp"
#include "connectcoin_pool.hpp"
#include "pow.hpp"
#include "randomx_hasher.hpp"
#include "warthog_miner.hpp"

#include <common/args.h>
#include <common/init.h>
#include <common/system.h>
#include <clientversion.h>
#include <chainparams.h>
#include <init.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <noui.h>
#include <util/chaintype.h>
#include <util/exception.h>
#include <util/signalinterrupt.h>
#include <util/threadnames.h>
#include <util/translation.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

const TranslateFn G_TRANSLATION_FUN{nullptr};

namespace {

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string processorDescription() {
#if defined(_WIN32)
    std::array<char, 256> value{};
    DWORD value_size{static_cast<DWORD>(value.size())};
    const LSTATUS result{RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString", RRF_RT_REG_SZ, nullptr, value.data(), &value_size)};
    if (result == ERROR_SUCCESS) {
        std::string description{value.data()};
        const auto last_character{description.find_last_not_of(' ')};
        if (last_character != std::string::npos) description.resize(last_character + 1);
        if (!description.empty()) return description;
    }
#endif
    return "Unknown CPU";
}

std::string toHex(const brminer::Hash256& hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash.size() * 2);
    for (const std::uint8_t byte : hash) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

int runPowSelfTest() {
    constexpr std::string_view key{"test key 000"};
    constexpr std::string_view input{"This is a test"};
    constexpr std::string_view expected{
        "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f"};

    const brminer::RandomXContext context(key);
    auto worker = context.createWorker();
    const auto hash = worker.hash(input.data(), input.size());
    if (toHex(hash) != expected) {
        throw std::runtime_error(
            "RandomX v2 known-answer test failed; the computed consensus hash did not match.");
    }
    std::cout << "RandomX v2 self-test passed: " << toHex(hash) << std::endl;
    return 0;
}

bool parseArguments(ArgsManager& args, int argc, char* argv[]) {
    SetupServerArgs(args);
    std::string error;
    if (!args.ParseParameters(argc, argv, error)) {
        InitError(Untranslated(strprintf("Error parsing command line arguments: %s", error)));
        return false;
    }
    if (auto config_error = common::InitConfig(args)) {
        InitError(config_error->message, config_error->details);
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (!IsSwitchChar(argv[i][0])) {
            InitError(Untranslated(strprintf(
                "Unexpected command-line token '%s'. Use -help to list supported Core options.",
                argv[i])));
            return false;
        }
    }
    return true;
}

int runNode(int argc, char* argv[], const std::string& payout_address,
            const std::string& thread_setting) {
    node::NodeContext context;
    int exit_status = EXIT_SUCCESS;
    auto init = interfaces::MakeNodeInit(context, argc, argv, exit_status);
    if (!init) return exit_status;

    SetupEnvironment();
    noui_connect();
    util::ThreadSetInternalName("init");

    auto app_node = init->makeNode();
    if (!app_node) throw std::runtime_error("ConnectCoin Core did not provide its node interface.");

    ArgsManager& args = *context.args;
    if (!parseArguments(args, argc, argv)) return EXIT_FAILURE;

    if (args.GetBoolArg("-help", false) || args.GetBoolArg("-?", false)) {
        std::cout << "Usage: brminer [ConnectCoin Core options]\n\n"
                  << args.GetHelpMessage() << std::endl;
        return EXIT_SUCCESS;
    }
    if (args.GetBoolArg("-version", false)) {
        std::cout << "BRminer embeds the pinned ConnectCoin Core node.\n";
        return EXIT_SUCCESS;
    }

    brminer::validateMiningSettings(payout_address, thread_setting);
    const int threads = brminer::parseThreadCount(thread_setting);

    args.ForceSetArg("-server", "0");
    args.ForceSetArg("-daemon", "0");
    args.ForceSetArg("-printtoconsole", "0");
    if (!args.IsArgSet("-onion")) {
        args.ForceSetArg("-onion", "0");
    }
    if (!args.IsArgSet("-listenonion")) {
        args.ForceSetArg("-listenonion", "0");
    }
    app_node->initLogging();
    app_node->initParameterInteraction();

    if (!app_node->baseInitialize() || !app_node->appInitMain()) {
        context.exit_status = EXIT_FAILURE;
        Interrupt(context);
        Shutdown(context);
        return app_node->getExitStatus();
    }

    try {
        app_node->startCpuMining(payout_address, threads);
    } catch (...) {
        Interrupt(context);
        Shutdown(context);
        throw;
    }

    std::cout << "BRminer: embedded ConnectCoin Core is running on chain "
              << args.GetChainTypeString() << "; CPU mining started for " << payout_address
              << " with " << threads << " worker(s). Press Ctrl+C to stop."
              << std::endl;

    const std::string processor{processorDescription()};
    const std::string core_version{FormatFullVersion()};
    constexpr int cpu_column_width{70};
    constexpr int hashrate_column_width{18};
    constexpr int blocks_column_width{12};
    const std::string table_rule(cpu_column_width + hashrate_column_width + blocks_column_width + 6, '-');
    std::cout << table_rule << '\n'
              << std::left << std::setw(cpu_column_width) << "CPU NAME"
              << " | " << std::setw(hashrate_column_width) << "HASHRATE"
              << " | " << std::setw(blocks_column_width) << "BLOCKS" << " |\n"
              << table_rule << std::endl;
    std::mutex status_mutex;
    std::condition_variable status_changed;
    std::atomic<bool> stop_status_monitor{false};
    std::atomic<bool> mining_failed{false};
    std::thread status_monitor;
    try {
        status_monitor = std::thread([&] {
            while (!stop_status_monitor.load()) {
                const auto status = app_node->getCpuMiningStatus();
                if (!status.error.empty()) {
                    std::cerr << "BRminer: CPU miner failed: " << status.error << std::endl;
                    mining_failed = true;
                    context.exit_status = EXIT_FAILURE;
                    app_node->startShutdown();
                    return;
                }
                if (!status.running && status.state == "stopped" &&
                    !app_node->shutdownRequested()) {
                    std::cerr << "BRminer: CPU miner stopped unexpectedly." << std::endl;
                    mining_failed = true;
                    context.exit_status = EXIT_FAILURE;
                    app_node->startShutdown();
                    return;
                }
                std::ostringstream hashrate;
                hashrate << std::fixed << std::setprecision(1) << status.hashes_per_second << " H/s";
                const int block_height{app_node->getNumBlocks()};
                int header_height{block_height};
                int64_t header_time{};
                const bool has_header{app_node->getHeaderTip(header_height, header_time)};
                const bool loading_blocks{app_node->isLoadingBlocks()};
                const bool initial_download{app_node->isInitialBlockDownload()};
                const int64_t spacing{Params().GetConsensus().PowTargetSpacing().count()};
                const int64_t now{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()};
                const bool estimate_header_progress{has_header && header_time > 0 &&
                    args.GetChainType() != ChainType::REGTEST};
                const int64_t seconds_behind{estimate_header_progress
                    ? std::max<int64_t>(0, now - header_time)
                    : 0};
                const int64_t estimated_blocks_left{spacing > 0 ? seconds_behind / spacing : 0};
                const int64_t estimated_header_height{estimate_header_progress
                    ? static_cast<int64_t>(header_height) + estimated_blocks_left
                    : 0};
                const double header_progress{!estimate_header_progress || estimated_header_height <= 0
                    ? 0.0
                    : std::clamp(100.0 * header_height / estimated_header_height, 0.0, 100.0)};
                const double block_progress{!has_header || header_height <= 0
                    ? 0.0
                    : std::clamp(100.0 * block_height / header_height, 0.0, 100.0)};
                const char* sync_phase{loading_blocks ? "loading chain" :
                    !initial_download ? "synced" :
                    !estimate_header_progress || header_progress < 99.9 ? "syncing headers" :
                    "downloading blocks"};
                std::cout << std::left << std::setw(cpu_column_width) << processor
                          << " | " << std::setw(hashrate_column_width) << hashrate.str()
                          << " | " << std::setw(blocks_column_width) << status.blocks << " |\n";
                if (loading_blocks || initial_download) {
                    std::cout << "Sync: " << sync_phase
                              << " | Headers: ";
                    if (estimate_header_progress) {
                        std::cout << std::fixed << std::setprecision(2) << header_progress
                                  << "% (" << header_height << "/~" << estimated_header_height << ")";
                    } else {
                        std::cout << "n/a (" << (has_header ? header_height : 0) << ")";
                    }
                    std::cout << " | Blocks: " << block_height << "/"
                              << (has_header ? header_height : 0) << " ("
                              << std::fixed << std::setprecision(1) << block_progress
                              << "% of known headers)\n";
                }
                std::cout << "Core " << core_version << " | Shares: 0 (solo) | Miner: "
                          << status.state << std::endl;
                std::unique_lock lock(status_mutex);
                status_changed.wait_for(lock, std::chrono::seconds(10), [&] {
                    return stop_status_monitor.load();
                });
            }
        });
    } catch (...) {
        Interrupt(context);
        Shutdown(context);
        throw;
    }

    (void)context.shutdown_signal->wait();
    stop_status_monitor = true;
    status_changed.notify_all();
    status_monitor.join();
    Interrupt(context);
    Shutdown(context);
    return mining_failed ? EXIT_FAILURE : context.exit_status.load();
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--embedded-core-check") {
            std::cout << "BRMINER_EMBEDDED_CORE=7" << std::endl;
            return EXIT_SUCCESS;
        }
        if (argc == 2 && std::string(argv[1]) == "--pow-self-test") {
            return runPowSelfTest();
        }

        const std::string payout_address = environment("BRMINER_PAYOUT_ADDRESS");
        const std::string thread_setting = environment("BRMINER_THREADS");
        const std::string node_setting = environment("BRMINER_WART_NODE");
        const std::string coin = environment("BRMINER_COIN");
        if (coin == "WART") {
            return brminer::warthog::runMiner(payout_address, thread_setting, node_setting);
        }
        if (coin == "CONNECTCOIN_POOL") {
            return brminer::connectcoin_pool::runMiner(payout_address, thread_setting);
        }
        return runNode(argc, argv, payout_address, thread_setting);
    } catch (const std::exception& error) {
        std::cerr << "BRminer: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
