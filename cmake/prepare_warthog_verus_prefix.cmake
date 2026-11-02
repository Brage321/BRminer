# Cache the Haraka state after the first 64 bytes of a Warthog header. Those
# bytes are unchanged while a worker checks nonces from the same Stratum job.
set(_verus_header "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.hpp")
set(_verus_source "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.cpp")
file(READ "${_verus_header}" _header)
string(REPLACE "\r\n" "\n" _header "${_header}")
set(_header_declaration "    VerusHasher& writeHeader80(const uint8_t* data);")
string(FIND "${_header}" "${_header_declaration}" _header_done)
if(_header_done EQUAL -1)
    set(_header_anchor "    VerusHasher& write(const uint8_t* data, size_t len);")
    string(FIND "${_header}" "${_header_anchor}" _header_pos)
    if(_header_pos EQUAL -1)
        message(FATAL_ERROR "Pinned JanusMiner VerusHasher header changed")
    endif()
    string(REPLACE "${_header_anchor}" "${_header_anchor}\n${_header_declaration}" _header "${_header}")
    file(WRITE "${_verus_header}" "${_header}")
endif()

file(READ "${_verus_source}" _source)
string(REPLACE "\r\n" "\n" _source "${_source}")
set(_source_anchor "VerusHasher& VerusHasher::write(const uint8_t* data, size_t len)")
string(FIND "${_source}" "VerusHasher& VerusHasher::writeHeader80(" _source_done)
if(_source_done EQUAL -1)
    string(FIND "${_source}" "${_source_anchor}" _source_pos)
    if(_source_pos EQUAL -1)
        message(FATAL_ERROR "Pinned JanusMiner VerusHasher source changed")
    endif()
    string(REPLACE "${_source_anchor}" [=[
VerusHasher& VerusHasher::writeHeader80(const uint8_t* data)
{
    struct PrefixCache {
        uint8_t input[64]{};
        uint8_t state[32]{};
        bool valid{};
        bool optimized{};
    };
    thread_local PrefixCache cache;
    if (!cache.valid || cache.optimized != optimized ||
        std::memcmp(cache.input, data, sizeof(cache.input)) != 0) {
        VerusHasher prefix;
        prefix.write(data, 64);
        std::memcpy(cache.input, data, sizeof(cache.input));
        std::memcpy(cache.state, prefix.curBuf, sizeof(cache.state));
        cache.optimized = optimized;
        cache.valid = true;
    }
    std::memcpy(curBuf, cache.state, sizeof(cache.state));
    std::memcpy(curBuf + 32, data + 64, 16);
    curPos = 16;
    return *this;
}

VerusHasher& VerusHasher::write(const uint8_t* data, size_t len)
]=] _source "${_source}")
    file(WRITE "${_verus_source}" "${_source}")
endif()
