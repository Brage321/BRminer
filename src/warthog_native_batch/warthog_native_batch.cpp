// Paired Verus batch hash adapted from the earlier local Warthog miner's
// native/wartproof.cpp. The referenced Janus and Verus notices are in LICENSES/.
// This variant records mutated key words for reverse restoration.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <openssl/sha.h>
#endif
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include "warthog_native_batch.hpp"
#include "crypto/verushash/verushash.hpp"
#include "crypto/verushash/verus_constants.hpp"


#ifndef WART_RESTORE_MODE
#define WART_RESTORE_MODE 0
#endif
#ifndef WART_LENGTH_HASH_MODE
#define WART_LENGTH_HASH_MODE 0
#endif
#ifndef WART_NATIVE_ARCH_ID
#define WART_NATIVE_ARCH_ID 0
#endif
#ifndef WART_NATIVE_LTCG
#define WART_NATIVE_LTCG 0
#endif
#ifndef WART_REJECT_BITS
#define WART_REJECT_BITS 16
#endif
#ifndef WART_SHARED_BACKUP
#define WART_SHARED_BACKUP 0
#endif
#ifndef WART_FAST_MODULO
#define WART_FAST_MODULO 0
#endif
#ifndef WART_INTERLEAVE2
#define WART_INTERLEAVE2 0
#endif
#if WART_RESTORE_MODE < 0 || WART_RESTORE_MODE > 2
#error WART_RESTORE_MODE must be 0 (sentinel), 1 (fixed), or 2 (unrolled)
#endif
#if WART_LENGTH_HASH_MODE < 0 || WART_LENGTH_HASH_MODE > 1
#error WART_LENGTH_HASH_MODE must be 0 (upstream) or 1 (exact constant)
#endif
#if WART_REJECT_BITS != 16 && WART_REJECT_BITS != 9
#error WART_REJECT_BITS must be 16 (verified baseline) or 9 (experimental)
#endif
#if WART_SHARED_BACKUP != 0 && WART_SHARED_BACKUP != 1
#error WART_SHARED_BACKUP must be 0 (private backup) or 1 (shared immutable backup)
#endif
#if WART_FAST_MODULO != 0 && WART_FAST_MODULO != 1
#error WART_FAST_MODULO must be 0 (upstream signed remainder) or 1 (unsigned limbs)
#endif
#if WART_INTERLEAVE2 != 0 && WART_INTERLEAVE2 != 1
#error WART_INTERLEAVE2 must be 0 (serial) or 1 (two independent round lanes)
#endif
#if WART_INTERLEAVE2 && !WART_SHARED_BACKUP
#error WART_INTERLEAVE2 requires WART_SHARED_BACKUP=1 to bound the key footprint
#endif
#if WART_NATIVE_ARCH_ID == 0
#define WART_NATIVE_ARCH_NAME "SSE2"
#elif WART_NATIVE_ARCH_ID == 1
#define WART_NATIVE_ARCH_NAME "AVX"
#elif WART_NATIVE_ARCH_ID == 2
#define WART_NATIVE_ARCH_NAME "AVX2"
#else
#error WART_NATIVE_ARCH_ID must be 0 (SSE2), 1 (AVX), or 2 (AVX2)
#endif
#define WART_STRINGIFY_IMPL(value) #value
#define WART_STRINGIFY(value) WART_STRINGIFY_IMPL(value)

#if WART_FAST_MODULO
#error The paired batch path uses the original signed remainder.
#endif
#define WART_REMAINDER64_32(numerator,divisor) ((numerator)%(divisor))
// These are upstream AES/CLMUL primitives, extracted by generate_primitives.py.
#ifdef _MSC_VER
#define __attribute__(x)
#endif
#define haraka256 brminer_native_haraka256
#define haraka512 brminer_native_haraka512
#define haraka512_keyed brminer_native_haraka512_keyed
#define verusclhash_sv2_1 brminer_native_verusclhash_sv2_1
#include "generated/aes_primitives.inc"

namespace {
uint32_t read_be32(const uint8_t* in) {
    return (uint32_t(in[0])<<24) | (uint32_t(in[1])<<16) |
           (uint32_t(in[2])<<8) | uint32_t(in[3]);
}
void write_be32(uint8_t* out, uint32_t value) {
    out[0]=uint8_t(value>>24); out[1]=uint8_t(value>>16);
    out[2]=uint8_t(value>>8); out[3]=uint8_t(value);
}

void prepare_state_and_key(const uint8_t* header76,uint8_t* state,uint8_t* key) {
    alignas(32) uint8_t buffer[64]{};
    std::memset(state,0,64);
    // Verus consumes 32-byte chunks. These results are invariant over nonces.
    std::memcpy(state+32,header76,32);
    haraka512(buffer,state);
    std::memcpy(buffer+32,header76+32,32);
    haraka512(state,buffer);
    std::memcpy(state+32,header76+64,12);
    std::memset(state+44,0,4);
    std::memcpy(state+48,state,16);
    const uint8_t* src=state;
    for(size_t i=0;i<Verus::key256blocks;++i) {
        uint8_t* dst=key+32*i;
        haraka256(dst,src);
        src=dst;
    }
}

#if WART_SHARED_BACKUP
// Published only through shared_ptr<const>. Construction completes under the
// cache mutex before any reader receives it. All SIMD inputs are aligned.
struct alignas(32) ImmutableJob {
    alignas(32) uint8_t key[Verus::keySizeInBytes];
    alignas(32) uint8_t state[64];
    std::array<uint8_t,76> prefix;

    explicit ImmutableJob(const uint8_t* header76) {
        std::memcpy(prefix.data(),header76,76);
        prepare_state_and_key(header76,state,key);
    }
};

std::shared_ptr<const ImmutableJob> cached_job(const uint8_t* header76) {
    // Four strong cache entries plus independently retained worker references.
    // Eviction never invalidates a worker's current immutable backup.
    struct JobCache {
        std::mutex mutex;
        std::array<std::shared_ptr<const ImmutableJob>,4> entries{};
        size_t next=0;
    };
    static JobCache cache;
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for(const auto& entry:cache.entries)
        if(entry && std::memcmp(entry->prefix.data(),header76,76)==0) return entry;
    std::shared_ptr<const ImmutableJob> result=std::make_shared<ImmutableJob>(header76);
    cache.entries[cache.next]=result;
    cache.next=(cache.next+1)%cache.entries.size();
    return result;
}
#endif

struct BatchHasher {
#if WART_SHARED_BACKUP
    alignas(32) uint8_t key[Verus::keySizeInBytes]{};
    alignas(32) __m128i old_words[wart_clhash_recorded_pointers]{};
    static constexpr size_t scratch_bytes=Verus::keySizeInBytes-Verus::keyRefreshsize;
    static_assert(scratch_bytes >= (wart_clhash_recorded_pointers+1)*sizeof(__m128i*));
    alignas(32) __m128i* mutation_scratch[scratch_bytes/sizeof(__m128i*)]{};
    std::shared_ptr<const ImmutableJob> job;
#else
    alignas(32) uint8_t key[2 * Verus::keySizeInBytes]{};
#endif
    alignas(32) uint8_t state[64]{};
    std::array<uint8_t,76> prefix{};
    bool initialized=false;

#if WART_SHARED_BACKUP
    void prepare_shared(std::shared_ptr<const ImmutableJob> next_job) {
        if(initialized && job.get()==next_job.get()) return;
        job=std::move(next_job);
        std::memcpy(state,job->state,sizeof(state));
        std::memcpy(key,job->key,sizeof(key));
        std::memset(mutation_scratch,0,sizeof(mutation_scratch));
        std::memcpy(prefix.data(),job->prefix.data(),prefix.size());
        initialized=true;
    }
#endif

    void restore_key(__m128i** scratch,bool logged) {
        if(logged) {
            // Reverse replay preserves duplicate-key mutation order exactly.
            for(size_t i=wart_clhash_recorded_pointers;i>0;--i)
                _mm_store_si128(scratch[i-1],old_words[i-1]);
            return;
        }
#if WART_SHARED_BACKUP
        // Explicit byte index into a separately allocated immutable key. Never
        // assume that a shared backup is adjacent to this thread's mutable key.
        const uint8_t* const backup=job->key;
        const uintptr_t primary=reinterpret_cast<uintptr_t>(key);
        const auto original=[backup,primary](__m128i* destination) {
            const uintptr_t location=reinterpret_cast<uintptr_t>(destination);
            assert(location>=primary);
            const size_t offset=size_t(location-primary);
            assert(offset%sizeof(__m128i)==0);
            assert(offset+sizeof(__m128i)<=Verus::keyRefreshsize);
            return _mm_load_si128(reinterpret_cast<const __m128i*>(backup+offset));
        };
#else
        constexpr size_t backup_offset=Verus::keySizeInBytes/sizeof(__m128i);
        const auto original=[](__m128i* destination) {
            return *(destination+backup_offset);
        };
#endif
#if WART_RESTORE_MODE == 0
        // Upstream sentinel traversal: preserved as the controlled A/B baseline.
        for (__m128i** entry=scratch; *entry; ++entry)
            **entry=original(*entry);
#elif WART_RESTORE_MODE == 1
        // The pinned sv2_2 routine always records two pointers per each of its
        // 32 rounds. generate_primitives.py validates that invariant.
        for (size_t i=0;i<wart_clhash_recorded_pointers;++i)
            *scratch[i]=original(scratch[i]);
#else
        static_assert(wart_clhash_recorded_pointers%4==0);
        for (size_t i=0;i<wart_clhash_recorded_pointers;i+=4) {
            __m128i* const p0=scratch[i];
            __m128i* const p1=scratch[i+1];
            __m128i* const p2=scratch[i+2];
            __m128i* const p3=scratch[i+3];
            // Backups occupy a distinct, immutable region. Duplicate mutation
            // addresses are safe: every store writes the identical original.
            const __m128i v0=original(p0);
            const __m128i v1=original(p1);
            const __m128i v2=original(p2);
            const __m128i v3=original(p3);
            *p0=v0; *p1=v1; *p2=v2; *p3=v3;
        }
#endif
    }

    void prepare(const uint8_t* header76) {
        if (initialized && std::memcmp(prefix.data(),header76,76)==0) return;
#if WART_SHARED_BACKUP
        // Assignment retains an owning reference while this worker uses key
        // and state; no other thread can mutate this worker's copies.
        prepare_shared(cached_job(header76));
#else
        prepare_state_and_key(header76,state,key);
        std::memcpy(key+Verus::keySizeInBytes,key,Verus::keyRefreshsize);
        std::memset(key+Verus::keySizeInBytes+Verus::keyRefreshsize,0,
                    Verus::keySizeInBytes-Verus::keyRefreshsize);
#endif
        // Publish the cache key only after a possibly allocating shared-cache
        // lookup has succeeded. A failed allocation must leave the old cached
        // prefix valid rather than associating an old key with a new header.
        std::memcpy(prefix.data(),header76,76);
        initialized=true;
    }

    __m128i** scratch() {
#if WART_SHARED_BACKUP
        return mutation_scratch;
#else
        return reinterpret_cast<__m128i**>(key+Verus::keySizeInBytes+Verus::keyRefreshsize);
#endif
    }

    void begin_nonce(uint32_t nonce) {
        write_be32(state+44,nonce);
        std::memcpy(state+48,state,16);
    }

    void finish_hash(__m128i acc,uint8_t* out32,bool logged=false) {
        auto* key128=reinterpret_cast<__m128i*>(key);
#if WART_LENGTH_HASH_MODE == 0
        acc=_mm_xor_si128(acc,lazyLengthHash(1024,64));
#else
        // Polynomial multiplication: 1024=x^10, 64=x^6, product=x^16.
        // Exactly equals lazyLengthHash(1024,64), without a per-nonce PCLMUL.
        acc=_mm_xor_si128(acc,_mm_set_epi64x(0,65536));
#endif
        const uint64_t intermediate=precompReduction64(acc);
        std::memcpy(state+48,&intermediate,8);
        std::memcpy(state+56,&intermediate,8);
        haraka512_keyed_local(out32,state,key128+(intermediate & Verus::keyMask16));
        // Restore exactly the mutated key entries, retaining the invariant key.
        restore_key(scratch(),logged);
    }

    void hash(uint32_t nonce,uint8_t* out32) {
        begin_nonce(nonce);
        const __m128i acc=__verusclmulwithoutreduction64alignedrepeat_sv2_2(
            reinterpret_cast<__m128i*>(key),reinterpret_cast<const __m128i*>(state),
            Verus::keyMask,scratch());
        finish_hash(acc,out32);
    }

};

#if WART_INTERLEAVE2
// Two complete mutable contexts; their only shared memory is const job material.
// No serial BatchHasher TLS object is instantiated by the pair scan/hash paths.
struct PairBatchHasher {
    BatchHasher first,second;

    void prepare(const uint8_t* header76) {
        first.prepare(header76);
        // One lookup, and exactly the same owning backup for both lanes even if
        // unrelated jobs concurrently churn the bounded global cache.
        second.prepare_shared(first.job);
    }

    void hash(uint32_t nonce,uint8_t* out32) { first.hash(nonce,out32); }

    void hash_pair(uint32_t nonce0,uint32_t nonce1,uint8_t* out0,uint8_t* out1) {
        first.begin_nonce(nonce0);
        second.begin_nonce(nonce1);
        __m128i acc0,acc1;
        wart_clhash_interleaved2_sv2_2(
            reinterpret_cast<__m128i*>(first.key),
            reinterpret_cast<const __m128i*>(first.state),first.scratch(),first.old_words,
            reinterpret_cast<__m128i*>(second.key),
            reinterpret_cast<const __m128i*>(second.state),second.scratch(),second.old_words,
            Verus::keyMask,acc0,acc1);
        first.finish_hash(acc0,out0,true);
        second.finish_hash(acc1,out1,true);
    }

};
using ActiveBatchHasher=PairBatchHasher;
#else
using ActiveBatchHasher=BatchHasher;
#endif

}

extern "C" int brminer_native_batch_hash_avx2(const uint8_t* header76,const uint32_t* nonces,
                                   size_t count,uint8_t* out_hashes32) {
    if(!header76 || (count && (!nonces || !out_hashes32)) || count>SIZE_MAX/32) return -1;
    try {
        thread_local ActiveBatchHasher hasher;
        hasher.prepare(header76);
#if WART_INTERLEAVE2
        size_t i=0;
        for(;i+1<count;i+=2)
            hasher.hash_pair(nonces[i],nonces[i+1],out_hashes32+32*i,out_hashes32+32*(i+1));
        if(i<count) hasher.hash(nonces[i],out_hashes32+32*i);
#else
        for(size_t i=0;i<count;++i) hasher.hash(nonces[i],out_hashes32+32*i);
#endif
        return 0;
    } catch(...) { return -3; }
}
