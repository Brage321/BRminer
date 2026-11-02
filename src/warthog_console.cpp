#include "warthog_console.hpp"

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace brminer::warthog {
namespace {

std::string cleanLine(std::string line) {
    for (auto& ch : line) if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = ' ';
    return line;
}

std::string timestamp() {
    const auto now{std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << '[' << std::put_time(&local, "%H:%M:%S") << "] ";
    return out.str();
}

} // namespace

struct MiningConsole::Impl {
    std::mutex mutex;
    MiningStats stats;
    std::string backend;
    std::uint32_t threads{};
    std::deque<std::string> events;
    bool live{};
    bool quiet{};
    int width{}, height{}, log_top{};
#if defined(_WIN32)
    HANDLE output{GetStdHandle(STD_OUTPUT_HANDLE)};
    DWORD original_mode{};
    bool changed_mode{};
#endif

    ~Impl() {
        if (live) std::cout << "\x1b[r" << std::flush;
#if defined(_WIN32)
        if (changed_mode) SetConsoleMode(output, original_mode);
#endif
    }

    std::vector<std::string> dashboard() const {
        std::string text{formatMiningTable(stats, backend, threads, width - 1)};
        if (width < 59 || height < 17) {
            text = "Warthog | " + backend + " | Up " + formatUptime(stats.uptime) + '\n' +
                "Janus " + formatHashrate(stats.janus) + " | Avg " + formatHashrate(stats.janus_average) + '\n' +
                "Accepted " + std::to_string(stats.accepted) + " | Rejected " +
                std::to_string(stats.rejected) + " | Blocks " + std::to_string(stats.blocks) + '\n';
        }
        if (height < 8)
            text = "Janus " + formatHashrate(stats.janus) + " | Up " + formatUptime(stats.uptime) + '\n';
        std::istringstream input{text};
        std::vector<std::string> lines;
        for (std::string line; std::getline(input, line); ) {
            if (line.empty() && lines.empty()) continue;
            lines.push_back(cleanLine(line).substr(0, std::max(1, width - 1)));
        }
        if (static_cast<int>(lines.size()) > height - 6) {
            lines = {"Warthog | " + backend + " | Up " + formatUptime(stats.uptime),
                "Janus " + formatHashrate(stats.janus) + " | Avg " + formatHashrate(stats.janus_average),
                "Accepted " + std::to_string(stats.accepted) + " | Rejected " +
                    std::to_string(stats.rejected) + " | Blocks " + std::to_string(stats.blocks)};
            for (auto& line : lines) line = line.substr(0, std::max(1, width - 1));
        }
        lines.push_back("");
        lines.push_back("Events (newest below)");
        return lines;
    }

    std::vector<std::string> eventLines(const std::string& event) const {
        const std::size_t length{static_cast<std::size_t>(std::max(1, width - 1))};
        std::vector<std::string> lines;
        for (std::size_t offset{}; offset < event.size(); offset += length)
            lines.push_back(event.substr(offset, length));
        return lines;
    }

    void paintDashboard() {
        const auto lines{dashboard()};
        for (std::size_t i{}; i < lines.size(); ++i)
            std::cout << "\x1b[" << i + 1 << ";1H\x1b[2K" << lines[i];
    }

    bool ensureLayout() {
        if (!live) return false;
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO screen{};
        if (!GetConsoleScreenBufferInfo(output, &screen)) return false;
        const int next_width{screen.srWindow.Right - screen.srWindow.Left + 1};
        const int next_height{screen.srWindow.Bottom - screen.srWindow.Top + 1};
#else
        winsize screen{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &screen) != 0) return false;
        const int next_width{screen.ws_col};
        const int next_height{screen.ws_row};
#endif
        if (next_width < 10 || next_height < 5) return false;
        const bool resized{next_width != width || next_height != height};
        width = next_width;
        height = next_height;
        const int next_log_top{static_cast<int>(dashboard().size()) + 1};
        if (!resized && next_log_top == log_top) return false;
        log_top = next_log_top;
        std::cout << "\x1b[r\x1b[2J\x1b[H";
        paintDashboard();
        // Only the event area scrolls; the table is outside these margins.
        std::cout << "\x1b[" << log_top << ';' << height << "r\x1b[" << log_top << ";1H";
        const std::size_t visible{static_cast<std::size_t>(std::max(0, height - log_top))};
        std::vector<std::string> wrapped;
        for (const auto& event : events) {
            const auto lines{eventLines(event)};
            wrapped.insert(wrapped.end(), lines.begin(), lines.end());
        }
        for (std::size_t i{wrapped.size() > visible ? wrapped.size() - visible : 0}; i < wrapped.size(); ++i)
            std::cout << wrapped[i] << '\n';
        std::cout << std::flush;
        return true;
    }

    void refresh() {
        if (!live || ensureLayout()) return;
        // Preserve the next event's position while overwriting the top rows.
        std::cout << "\x1b" "7";
        paintDashboard();
        std::cout << "\x1b" "8" << std::flush;
    }
};

MiningConsole::MiningConsole() : impl{std::make_unique<Impl>()} {}
MiningConsole::~MiningConsole() = default;

void MiningConsole::start(std::string backend, std::uint32_t threads, bool diagnostics, bool quiet) {
    std::lock_guard lock{impl->mutex};
    impl->backend = std::move(backend);
    impl->threads = threads;
    impl->quiet = quiet;
#if defined(_WIN32)
    if (!diagnostics && !quiet && GetConsoleMode(impl->output, &impl->original_mode)) {
        impl->live = SetConsoleMode(impl->output,
            impl->original_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        impl->changed_mode = impl->live;
    }
#else
    const char* term{std::getenv("TERM")};
    impl->live = !diagnostics && !quiet && isatty(STDOUT_FILENO) && term && std::string{term} != "dumb";
#endif
    impl->ensureLayout();
}

void MiningConsole::update(const MiningStats& stats) {
    std::lock_guard lock{impl->mutex};
    const auto accepted{impl->stats.accepted}, rejected{impl->stats.rejected}, blocks{impl->stats.blocks};
    impl->stats = stats;
    impl->stats.accepted = std::max(accepted, stats.accepted);
    impl->stats.rejected = std::max(rejected, stats.rejected);
    impl->stats.blocks = std::max(blocks, stats.blocks);
    if (impl->quiet) return;
    if (impl->live) impl->refresh();
    else std::cout << formatMiningTable(impl->stats, impl->backend, impl->threads) << std::flush;
}

void MiningConsole::shares(std::uint64_t accepted, std::uint64_t rejected, std::uint64_t blocks) {
    std::lock_guard lock{impl->mutex};
    impl->stats.accepted = accepted;
    impl->stats.rejected = rejected;
    impl->stats.blocks = blocks;
    impl->refresh();
}

void MiningConsole::event(const std::string& message) {
    std::lock_guard lock{impl->mutex};
    if (impl->quiet) return;
    impl->ensureLayout();
    const std::string line{timestamp() + cleanLine(message)};
    impl->events.push_back(line);
    if (impl->events.size() > 1000) impl->events.pop_front();
    if (impl->live) {
        for (const auto& part : impl->eventLines(line)) std::cout << part << '\n';
    } else std::cout << line << '\n';
    std::cout << std::flush;
}

void MiningConsole::diagnostic(const std::string& line) {
    std::lock_guard lock{impl->mutex};
    if (impl->quiet) return;
    std::cout << line << '\n' << std::flush;
}

bool MiningConsole::isLive() const { return impl->live; }

} // namespace brminer::warthog
