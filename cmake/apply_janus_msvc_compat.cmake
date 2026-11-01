set(_janus_verushash "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verushash.cpp")
set(_janus_clhash_port "${janusminer_source_SOURCE_DIR}/src/shared/src/crypto/verushash/verus_clhash_port.cpp")

foreach(_source IN ITEMS "${_janus_verushash}" "${_janus_clhash_port}")
	if(NOT EXISTS "${_source}")
		message(FATAL_ERROR "Pinned JanusHash source file was not found: ${_source}")
	endif()
endforeach()

file(READ "${_janus_verushash}" _verushash)
string(REPLACE "#include \"spdlog/spdlog.h\"\n" "" _verushash "${_verushash}")

set(_include_anchor [=[#if defined(__arm__) || defined(__aarch64__)
#include "crypto/sse2neon.h"]=])
set(_include_replacement [=[#if defined(_MSC_VER)
#include <immintrin.h>
#elif defined(__arm__) || defined(__aarch64__)
#include "crypto/sse2neon.h"]=])
string(FIND "${_verushash}" "${_include_anchor}" _include_index)
if(NOT _include_index EQUAL -1)
	string(REPLACE "${_include_anchor}" "${_include_replacement}" _verushash "${_verushash}")
else()
	string(FIND "${_verushash}" "#if defined(_MSC_VER)\n#include <immintrin.h>" _patched_include_index)
	if(_patched_include_index EQUAL -1)
		message(FATAL_ERROR "Could not apply the MSVC include patch to pinned JanusHash Verushash source")
	endif()
endif()

set(_cpuid_anchor [=[bool can_optimize()
{
#if defined(__arm__) || defined(__aarch64__)]=])
set(_cpuid_replacement [=[bool can_optimize()
{
#if defined(_MSC_VER)
	return false;
#elif defined(__arm__) || defined(__aarch64__)]=])
string(FIND "${_verushash}" "${_cpuid_anchor}" _cpuid_index)
if(NOT _cpuid_index EQUAL -1)
	string(REPLACE "${_cpuid_anchor}" "${_cpuid_replacement}" _verushash "${_verushash}")
else()
	string(FIND "${_verushash}" "bool can_optimize()\n{\n#if defined(_MSC_VER)\n\treturn false;" _patched_cpuid_index)
	if(_patched_cpuid_index EQUAL -1)
		message(FATAL_ERROR "Could not apply the MSVC CPU-detection patch to pinned JanusHash source")
	endif()
endif()
file(WRITE "${_janus_verushash}" "${_verushash}")

file(READ "${_janus_clhash_port}" _clhash_port)
set(_attribute_anchor [=[static inline __attribute__((always_inline)) void
haraka512_port_keyed_local]=])
set(_attribute_replacement [=[static inline  void
haraka512_port_keyed_local]=])
string(FIND "${_clhash_port}" "${_attribute_anchor}" _attribute_index)
if(NOT _attribute_index EQUAL -1)
	string(REPLACE "${_attribute_anchor}" "${_attribute_replacement}" _clhash_port "${_clhash_port}")
elseif(NOT _clhash_port MATCHES "static inline  void[\r\n]+haraka512_port_keyed_local")
	message(FATAL_ERROR "Could not apply the MSVC attribute patch to pinned JanusHash source")
endif()
file(WRITE "${_janus_clhash_port}" "${_clhash_port}")
