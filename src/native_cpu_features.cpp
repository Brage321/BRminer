#include "warthog_native_batch/warthog_native_batch.hpp"

#include <cstdlib>
#include <limits>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

// This translation unit is intentionally compiled without AVX/AVX2 flags.
extern "C" int brminer_native_cpu_supported(void) {
    static const bool supported = [] {
        if (const char* disabled = std::getenv("BRMINER_DISABLE_SIMD");
            disabled && disabled[0] == '1' && disabled[1] == '\0') return false;
        unsigned ecx{}, ebx{};
#if defined(_MSC_VER)
        int regs[4]{};
        __cpuidex(regs, 1, 0);
        ecx = static_cast<unsigned>(regs[2]);
#else
        unsigned eax{}, edx{};
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
#endif
        constexpr unsigned required{(1U << 1) | (1U << 9) | (1U << 25) | (1U << 27) | (1U << 28)};
        if ((ecx & required) != required) return false;
#if defined(_MSC_VER)
        if ((_xgetbv(0) & 6) != 6) return false;
        __cpuidex(regs, 7, 0);
        ebx = static_cast<unsigned>(regs[1]);
#else
        unsigned xsave_low{}, xsave_high{};
        __asm__ volatile("xgetbv" : "=a"(xsave_low), "=d"(xsave_high) : "c"(0));
        if ((xsave_low & 6) != 6) return false;
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
#endif
        return (ebx & (1U << 5)) != 0;
    }();
    return supported;
}

extern "C" int brminer_native_batch_hash_avx2(const std::uint8_t*, const std::uint32_t*, std::size_t, std::uint8_t*);

extern "C" int brminer_native_batch_hash(const std::uint8_t* header, const std::uint32_t* nonces,
    std::size_t count, std::uint8_t* hashes) {
    if (!header || (count && (!nonces || !hashes)) || count > std::numeric_limits<std::size_t>::max() / 32) return -1;
    if (!brminer_native_cpu_supported()) return -2;
    return brminer_native_batch_hash_avx2(header, nonces, count, hashes);
}
