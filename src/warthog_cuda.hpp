#pragma once

#include "warthog_pow.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brminer::warthog {

// An explicit environment override, then the bundled compiler beside the executable.
std::string findNvrtcDll();
std::vector<std::string> nvrtcLibraries();

struct CudaDeviceIdentity {
    std::string name, uuid;
    std::string module_mode;
    int compute_capability{}, driver_version{}, compiler_version{};
    std::uint64_t initialization_nanos{};
};

struct CudaLinkBandwidth {
    double host_to_device_bytes_s{}, device_to_host_bytes_s{};
};

struct CudaPipelineResult {
	std::uint64_t tag{};
	std::uint32_t nonce_start{}, nonce_count{};
	std::vector<Sha256tCandidate> candidates;
};

struct CudaPipelineStats {
	std::uint64_t batches_submitted{}, batches_completed{}, kernel_launches{};
	std::uint64_t overflow_retries{}, buffer_growths{}, host_wait_nanos{};
};

// Optional NVIDIA CUDA Driver/NVRTC path. The OpenCL miner remains available
// when the compiler DLL or a supported CUDA device is unavailable.
class CudaSha256tMiner {
public:
	static std::vector<std::string> devices(const std::string& nvrtc_dll);
	static std::vector<CudaDeviceIdentity> identities(const std::string& nvrtc_library);

        CudaSha256tMiner(std::size_t device_index, const std::string& nvrtc_dll,
            std::uint32_t block_size = 128);
	~CudaSha256tMiner();
	CudaSha256tMiner(const CudaSha256tMiner&) = delete;
	CudaSha256tMiner& operator=(const CudaSha256tMiner&) = delete;

	std::vector<Sha256tCandidate> scan(const Header& header,
		std::uint32_t nonce_start, std::uint32_t nonce_count,
		std::uint32_t candidate_threshold);
	std::optional<CudaPipelineResult> submitPipelined(const Header& header,
		std::uint32_t nonce_start, std::uint32_t nonce_count,
		std::uint32_t candidate_threshold, std::uint64_t tag);
	std::optional<CudaPipelineResult> harvestPipelined();
	CudaPipelineStats pipelineStats() const;
	const std::string& deviceName() const;
	const CudaDeviceIdentity& identity() const;
	void setBlockSize(std::uint32_t block_size);
	CudaLinkBandwidth measureBandwidth(std::size_t bytes = 64 * 1024 * 1024);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

}  // namespace brminer::warthog
