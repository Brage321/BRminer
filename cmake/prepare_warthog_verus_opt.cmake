# The pinned JanusMiner dependency includes x86 AES/PCLMUL Verus primitives,
# but its public VerusHasher currently calls only the portable implementation.
# Build those primitives separately and select them at runtime on capable CPUs.
set(BRMINER_VERUS_OPT_SOURCE "")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR (NOT MSVC AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    return()
endif()

set(_opt_input "${janusminer_source_SOURCE_DIR}/src/miner/verusopt/verus_clhash_opt.cpp")
set(_verushash_input "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.cpp")
file(READ "${_opt_input}" _opt)
string(REPLACE "\r\n" "\n" _opt "${_opt}")
string(FIND "${_opt}" "namespace {\n\n#define LOAD" _opt_begin)
string(FIND "${_opt}" "namespace Verus {\nnamespace {" _opt_end)
if(_opt_begin EQUAL -1 OR _opt_end EQUAL -1 OR _opt_end LESS _opt_begin)
    message(FATAL_ERROR "Pinned JanusMiner optimized Verus source changed unexpectedly")
endif()
math(EXPR _opt_length "${_opt_end} - ${_opt_begin}")
string(SUBSTRING "${_opt}" ${_opt_begin} ${_opt_length} _opt_body)
string(REPLACE "__attribute__((always_inline))" "" _opt_body "${_opt_body}")
# The SIMD CLHash core mutates two key words in each of 32 rounds. Save the
# pre-mutation words in the key's otherwise unused refresh area and replay the
# writes backward, so two mining threads share less L1 data than with an 8 KiB
# reference key copy. The portable path retains its original refresh copy.
string(FIND "${_opt_body}" "        *(pMoveScratch++) = prand;\n        *(pMoveScratch++) = prandex;" _mutation_log_site)
if(_mutation_log_site EQUAL -1)
    message(FATAL_ERROR "Pinned JanusMiner CLHash mutation sites changed unexpectedly")
endif()
string(REPLACE "    for (int64_t i = 0; i < 32; i++) {"
    "    __m128i* brminer_old_key_words = reinterpret_cast<__m128i*>(\n        reinterpret_cast<uint8_t*>(pMoveScratch) - Verus::keyRefreshsize);\n    unsigned int brminer_mutation_count = 0;\n    for (int64_t i = 0; i < 32; i++) {"
    _opt_body "${_opt_body}")
string(REPLACE "_mm_load_si128(prand)" "brminer_original_prand" _opt_body "${_opt_body}")
string(REPLACE "_mm_load_si128(prandex)" "brminer_original_prandex" _opt_body "${_opt_body}")
string(REPLACE "        *(pMoveScratch++) = prand;\n        *(pMoveScratch++) = prandex;"
    "        const __m128i brminer_original_prand = _mm_load_si128(prand);\n        const __m128i brminer_original_prandex = _mm_load_si128(prandex);\n        brminer_old_key_words[brminer_mutation_count++] = brminer_original_prand;\n        brminer_old_key_words[brminer_mutation_count++] = brminer_original_prandex;\n        *(pMoveScratch++) = prand;\n        *(pMoveScratch++) = prandex;"
    _opt_body "${_opt_body}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(BRMINER_VERUS_OPT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/brminer_verus_opt.cpp")
file(WRITE "${BRMINER_VERUS_OPT_SOURCE}" [=[
#include "crypto/verushash/u128.h"
#include "crypto/verushash/verus_constants.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

]=] "${_opt_body}" [=[
void verus_restore_mutations(__m128i** pointers)
{
    if (pointers[0] == nullptr) return;
    const __m128i* old_key_words = reinterpret_cast<const __m128i*>(
        reinterpret_cast<const uint8_t*>(pointers) - Verus::keyRefreshsize);
    for (int i = 63; i >= 0; --i) {
        _mm_store_si128(pointers[i], old_key_words[i]);
    }
    pointers[0] = nullptr;
}

uint64_t verusclhash_sv2_2(void* random, const unsigned char buf[64],
    uint64_t keyMask, __m128i** pMoveScratch)
{
    __m128i acc = __verusclmulwithoutreduction64alignedrepeat_sv2_2(
        (__m128i*)random, (const __m128i*)buf, keyMask, pMoveScratch);
    acc = _mm_xor_si128(acc, lazyLengthHash(1024, 64));
    return precompReduction64(acc);
}
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/brminer_verus_opt.hpp" [=[
#pragma once
#include "crypto/verushash/u128.h"
#include <cstdint>
void haraka256(unsigned char* out, const unsigned char* in);
void haraka512(unsigned char* out, const unsigned char* in);
void haraka512_keyed(unsigned char* out, const unsigned char* in, const u128* rc);
void verus_restore_mutations(__m128i** pointers);
uint64_t verusclhash_sv2_1(void* random, const unsigned char buf[64],
    uint64_t keyMask, __m128i** pMoveScratch);
uint64_t verusclhash_sv2_2(void* random, const unsigned char buf[64],
    uint64_t keyMask, __m128i** pMoveScratch);
]=])

file(READ "${_verushash_input}" _verushash)
string(REPLACE "\r\n" "\n" _verushash "${_verushash}")
string(FIND "${_verushash}" "brminer_disable_simd" _disable_guard)
if(_disable_guard EQUAL -1)
    string(PREPEND _verushash "#include <cstdlib>\n")
    string(REPLACE "bool can_optimize()\n{"
        "bool can_optimize()\n{\n    static const bool brminer_disable_simd = [] {\n        const char* value = std::getenv(\"BRMINER_DISABLE_SIMD\");\n        return value && value[0] == '1' && value[1] == '\\0';\n    }();\n    if (brminer_disable_simd) return false;"
        _verushash "${_verushash}")
endif()
if(NOT MSVC)
    string(REPLACE "return ((ecx & (bit_AVX | bit_AES | bit_PCLMUL)) == (bit_AVX | bit_AES | bit_PCLMUL));"
        "return __builtin_cpu_supports(\"avx2\") && __builtin_cpu_supports(\"aes\") && __builtin_cpu_supports(\"pclmul\") && __builtin_cpu_supports(\"ssse3\");"
        _verushash "${_verushash}")
endif()
string(REPLACE "#include \"brminer_verus_opt.hpp\"\n" "" _verushash "${_verushash}")
function(_brminer_replace_checked old new)
    string(FIND "${_verushash}" "${new}" _already_patched)
    if(NOT _already_patched EQUAL -1)
        return()
    endif()
    string(FIND "${_verushash}" "${old}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "Pinned JanusMiner VerusHasher changed; could not apply SIMD dispatch patch")
    endif()
    string(REPLACE "${old}" "${new}" _verushash "${_verushash}")
    set(_verushash "${_verushash}" PARENT_SCOPE)
endfunction()

_brminer_replace_checked(
    "#include \"verus_clhash_port.hpp\""
    "#include \"verus_clhash_port.hpp\"\n#include \"brminer_verus_opt.hpp\"")
_brminer_replace_checked(
    "    inline void fixupkey()\n    {"
    "    inline void fixupkey()\n    {\n        if (fast_restore) {\n            verus_restore_mutations(getpmovescratch());\n            return;\n        }")
_brminer_replace_checked(
    "    Hash curSeed;"
    "    Hash curSeed;\n    bool fast_restore{};")
_brminer_replace_checked(
    "    : curSeed(seed)"
    "    : curSeed(seed), fast_restore(haraka256Function == haraka256)")
string(FIND "${_verushash}" "#if defined(_MSC_VER)\n\treturn false;\n#elif defined(__arm__)" _original_dispatch)
if(NOT _original_dispatch EQUAL -1)
    _brminer_replace_checked(
        "#if defined(_MSC_VER)\n\treturn false;\n#elif defined(__arm__)"
        "#if defined(_MSC_VER)\n    static const bool supported = [] {\n        int regs[4]{};\n        __cpuid(regs, 1);\n        constexpr int required = (1 << 1) | (1 << 9) | (1 << 25);\n        return (regs[2] & required) == required;\n    }();\n    return supported;\n#elif defined(__arm__)")
endif()
set(_brminer_legacy_cpu_check "constexpr int required = (1 << 1) | (1 << 9) | (1 << 25);\n        return (regs[2] & required) == required;")
set(_brminer_avx_cpu_check "constexpr int required = (1 << 1) | (1 << 9) | (1 << 25) | (1 << 27) | (1 << 28);\n        return (regs[2] & required) == required && (_xgetbv(0) & 0x6) == 0x6;")
set(_brminer_avx2_cpu_check "constexpr int required = (1 << 1) | (1 << 9) | (1 << 25) | (1 << 27) | (1 << 28);\n        if ((regs[2] & required) != required || (_xgetbv(0) & 0x6) != 0x6) return false;\n        __cpuidex(regs, 7, 0);\n        return (regs[1] & (1 << 5)) != 0;")
set(_brminer_bmi2_cpu_check "constexpr int required = (1 << 1) | (1 << 9) | (1 << 25) | (1 << 27) | (1 << 28);\n        if ((regs[2] & required) != required || (_xgetbv(0) & 0x6) != 0x6) return false;\n        __cpuidex(regs, 7, 0);\n        return (regs[1] & ((1 << 5) | (1 << 8))) == ((1 << 5) | (1 << 8));")
set(_brminer_vaes_cpu_check "constexpr int required = (1 << 1) | (1 << 9) | (1 << 25) | (1 << 27) | (1 << 28);\n        if ((regs[2] & required) != required || (_xgetbv(0) & 0x6) != 0x6) return false;\n        __cpuidex(regs, 7, 0);\n        return (regs[1] & (1 << 5)) != 0 && (regs[2] & (1 << 9)) != 0;")
if(BRMINER_CLANG_CL)
    set(_brminer_cpu_check "${_brminer_avx2_cpu_check}")
else()
    set(_brminer_cpu_check "${_brminer_avx_cpu_check}")
endif()
string(FIND "${_verushash}" "${_brminer_cpu_check}" _brminer_dispatch_ready)
if(_brminer_dispatch_ready EQUAL -1)
    foreach(_brminer_old_check IN ITEMS "${_brminer_legacy_cpu_check}" "${_brminer_avx_cpu_check}" "${_brminer_avx2_cpu_check}" "${_brminer_bmi2_cpu_check}" "${_brminer_vaes_cpu_check}")
        string(FIND "${_verushash}" "${_brminer_old_check}" _brminer_old_found)
        if(NOT _brminer_old_found EQUAL -1)
            string(REPLACE "${_brminer_old_check}" "${_brminer_cpu_check}" _verushash "${_verushash}")
            set(_brminer_dispatch_ready 0)
            break()
        endif()
    endforeach()
endif()
if(_brminer_dispatch_ready EQUAL -1)
    message(FATAL_ERROR "Pinned JanusMiner CPU feature dispatch changed unexpectedly")
endif()
_brminer_replace_checked(
    "HashKey hk(curBuf, haraka256_port);"
    "HashKey hk(curBuf, optimized ? haraka256 : haraka256_port);")
_brminer_replace_checked(
    "    HashKey hk(curBuf, optimized ? haraka256 : haraka256_port);"
    "    thread_local HashKey hk(curBuf, optimized ? haraka256 : haraka256_port);\n    hk.update_seed(curBuf, optimized ? haraka256 : haraka256_port);")
string(REPLACE "    if (fast_restore) verus_reset_mutation_log();\n" ""
    _verushash "${_verushash}")
_brminer_replace_checked(
    "void HashKey::apply_seed(HashView newSeed,\n    void (*haraka256Function)(unsigned char* out,\n        const unsigned char* in))\n{\n    unsigned char* pkey = key;"
    "void HashKey::apply_seed(HashView newSeed,\n    void (*haraka256Function)(unsigned char* out,\n        const unsigned char* in))\n{\n    curSeed = Hash{newSeed};\n    unsigned char* pkey = key;")
_brminer_replace_checked(
    "    } else {\n        memcpy(key, key + keySizeInBytes, keyRefreshsize);\n        memset((unsigned char*)key + (keySizeInBytes + keyRefreshsize), 0,\n            keySizeInBytes - keyRefreshsize);\n    }\n};"
    "    } else {\n        fixupkey();\n    }\n};")
_brminer_replace_checked(
    "curBuf, use_2_2 ? verusclhash_sv2_2_port : verusclhash_sv2_1_port)"
    "curBuf, optimized ? (use_2_2 ? verusclhash_sv2_2 : verusclhash_sv2_1)\n                          : (use_2_2 ? verusclhash_sv2_2_port : verusclhash_sv2_1_port))")
_brminer_replace_checked(
    "haraka512_port_keyed(out.data(), curBuf,\n        (const u128*)hk.key_data() + (intermediate & mask16));"
    "(optimized ? haraka512_keyed : haraka512_port_keyed)(out.data(), curBuf,\n        (const u128*)hk.key_data() + (intermediate & mask16));")
_brminer_replace_checked(
    "return write(data, len, haraka512_port);"
    "return write(data, len, optimized ? haraka512 : haraka512_port);")
file(WRITE "${_verushash_input}" "${_verushash}")
