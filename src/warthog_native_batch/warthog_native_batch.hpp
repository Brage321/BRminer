#pragma once
#include <cstddef>
#include <cstdint>
extern "C" int brminer_native_cpu_supported(void);
extern "C" int brminer_native_batch_hash(const std::uint8_t* header76, const std::uint32_t* nonces,
    std::size_t count, std::uint8_t* out_hashes32);
