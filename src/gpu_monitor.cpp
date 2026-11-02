#include "gpu_monitor.hpp"
#include "platform.hpp"

#include <array>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace brminer {

std::string clockReasonText(std::optional<std::uint64_t> reasons) {
    if (!reasons) return "N/A";
    if (*reasons == 0) return "none";
    constexpr std::array<std::pair<std::uint64_t, const char*>, 9> names{{
        {1, "idle"}, {2, "application clocks"}, {4, "power cap"}, {8, "hardware slowdown"},
        {16, "sync boost"}, {32, "software thermal"}, {64, "hardware thermal"},
        {128, "power brake"}, {256, "display clocks"}}};
    std::string text;
    std::uint64_t remaining{*reasons};
    for (const auto& [bit, name] : names) if (remaining & bit) {
        if (!text.empty()) text += ", ";
        text += name;
        remaining &= ~bit;
    }
    if (remaining) {
        std::ostringstream value;
        value << "0x" << std::hex << remaining;
        if (!text.empty()) text += ", ";
        text += value.str();
    }
    return text;
}

struct NvidiaMonitor::Impl {
    using Device = void*;
    struct Utilization { unsigned gpu, memory; };
    DynamicLibrary library;
    bool initialized{};
    std::string error;
    std::string driver;
    std::vector<Device> devices;
    int (*shutdown)(){};
    int (*temperature)(Device, unsigned, unsigned*){};
    int (*utilization)(Device, Utilization*){};
    int (*power)(Device, unsigned*){};
    int (*clock)(Device, unsigned, unsigned*){};
    int (*reasons)(Device, unsigned long long*){};
    int (*name)(Device, char*, unsigned){};
    int (*uuid)(Device, char*, unsigned){};

    template<typename Function> Function symbol(const char* name) const {
        return reinterpret_cast<Function>(library.symbol(name));
    }

    Impl() {
        if (environmentValue("BRMINER_DISABLE_NVML") == "1") { error = "disabled"; return; }
        try {
#if defined(_WIN32)
            library = DynamicLibrary{"nvml.dll", true};
#else
            library = DynamicLibrary{"libnvidia-ml.so.1", true};
#endif
            const auto init{symbol<int (*)()>("nvmlInit_v2")};
            shutdown = symbol<decltype(shutdown)>("nvmlShutdown");
            const auto count{symbol<int (*)(unsigned*)>("nvmlDeviceGetCount_v2")};
            const auto device{symbol<int (*)(unsigned, Device*)>("nvmlDeviceGetHandleByIndex_v2")};
            if (!init || !shutdown || !count || !device || init() != 0)
                throw std::runtime_error("NVML initialization unavailable");
            initialized = true;
            unsigned total{};
            if (count(&total) != 0) throw std::runtime_error("NVML device enumeration unavailable");
            for (unsigned i{}; i < total; ++i) {
                Device handle{};
                if (device(i, &handle) == 0) devices.push_back(handle);
            }
            temperature = symbol<decltype(temperature)>("nvmlDeviceGetTemperature");
            utilization = symbol<decltype(utilization)>("nvmlDeviceGetUtilizationRates");
            power = symbol<decltype(power)>("nvmlDeviceGetPowerUsage");
            clock = symbol<decltype(clock)>("nvmlDeviceGetClockInfo");
            reasons = symbol<decltype(reasons)>("nvmlDeviceGetCurrentClocksEventReasons");
            if (!reasons) reasons = symbol<decltype(reasons)>("nvmlDeviceGetCurrentClocksThrottleReasons");
            name = symbol<decltype(name)>("nvmlDeviceGetName");
            uuid = symbol<decltype(uuid)>("nvmlDeviceGetUUID");
            const auto version{symbol<int (*)(char*, unsigned)>("nvmlSystemGetDriverVersion")};
            std::array<char, 96> text{};
            if (version && version(text.data(), static_cast<unsigned>(text.size())) == 0) driver = text.data();
        } catch (const std::exception& exception) { error = exception.what(); devices.clear(); }
    }

    ~Impl() { if (initialized && shutdown) shutdown(); }
};

NvidiaMonitor::NvidiaMonitor() : impl{std::make_unique<Impl>()} {}
NvidiaMonitor::~NvidiaMonitor() = default;
bool NvidiaMonitor::available() const { return !impl->devices.empty(); }
const std::string& NvidiaMonitor::unavailableReason() const { return impl->error; }

std::vector<GpuTelemetry> NvidiaMonitor::sample() const {
    std::vector<GpuTelemetry> samples;
    for (const auto device : impl->devices) {
        GpuTelemetry sample;
        sample.driver = impl->driver;
        std::array<char, 256> text{};
        if (impl->name && impl->name(device, text.data(), static_cast<unsigned>(text.size())) == 0) sample.name = text.data();
        if (impl->uuid && impl->uuid(device, text.data(), static_cast<unsigned>(text.size())) == 0) sample.uuid = text.data();
        unsigned value{};
        if (impl->temperature && impl->temperature(device, 0, &value) == 0) sample.temperature_c = value;
        Impl::Utilization use{};
        if (impl->utilization && impl->utilization(device, &use) == 0) sample.utilization_pct = use.gpu;
        if (impl->power && impl->power(device, &value) == 0) sample.power_w = value / 1000.0;
        if (impl->clock && impl->clock(device, 0, &value) == 0) sample.core_mhz = value;
        if (impl->clock && impl->clock(device, 2, &value) == 0) sample.memory_mhz = value;
        unsigned long long reasons{};
        if (impl->reasons && impl->reasons(device, &reasons) == 0) sample.clock_reasons = reasons;
        samples.push_back(std::move(sample));
    }
    return samples;
}

} // namespace brminer
