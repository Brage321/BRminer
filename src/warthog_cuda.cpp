#include "warthog_cuda.hpp"

#include "warthog_gpu.hpp"
#include "warthog_cuda_kernel.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace brminer::warthog {

std::vector<std::string> nvrtcLibraries() {
	for (const auto* name : {"BRMINER_NVRTC_DLL", "BRMINER_NVRTC_LIBRARY"})
		if (const char* override_path = std::getenv(name); override_path && *override_path) return {override_path};
	const auto directory{executableDirectory()};
	std::vector<std::filesystem::path> locations{directory / "cuda", directory / "cuda/legacy", directory};
#if defined(_WIN32)
	const auto installed{environmentValue("CUDA_PATH")};
	if (!installed.empty()) {
		locations.push_back(std::filesystem::path{installed} / "bin/x64");
		locations.push_back(std::filesystem::path{installed} / "bin");
	}
	const auto program_files{environmentValue("ProgramFiles")};
	const auto toolkit_root{std::filesystem::path{program_files} / "NVIDIA GPU Computing Toolkit/CUDA"};
	std::error_code toolkit_error;
	std::vector<std::filesystem::path> toolkits;
	if (!program_files.empty() && std::filesystem::is_directory(toolkit_root, toolkit_error)) {
		for (const auto& entry : std::filesystem::directory_iterator(toolkit_root, toolkit_error))
			if (entry.is_directory(toolkit_error)) toolkits.push_back(entry.path());
		const auto version = [](const auto& path) {
			try {
				const auto text{path.filename().string().substr(1)};
				const auto dot{text.find('.')};
				return std::pair{std::stoi(text.substr(0, dot)), dot == std::string::npos ? 0 : std::stoi(text.substr(dot + 1))};
			} catch (...) { return std::pair{0, 0}; }
		};
		std::sort(toolkits.begin(), toolkits.end(), [&](const auto& a, const auto& b) { return version(a) > version(b); });
		for (const auto& toolkit : toolkits) {
			locations.push_back(toolkit / "bin/x64");
			locations.push_back(toolkit / "bin");
		}
	}
	locations.push_back(directory / "out/toolchain/cuda/extracted/nvidia/cuda_nvrtc/bin");
	const std::array<const char*, 2> names{"nvrtc64_130_0.dll", "nvrtc64_120_0.dll"};
#else
	const auto installed{environmentValue("CUDA_HOME")};
	if (!installed.empty()) locations.push_back(std::filesystem::path{installed} / "lib64");
	locations.push_back("/usr/local/cuda/lib64");
	locations.push_back("/usr/lib/x86_64-linux-gnu");
	const std::array<const char*, 3> names{"libnvrtc.so.13", "libnvrtc.so.12", "libnvrtc.so"};
#endif
	std::vector<std::string> found;
	for (const auto& location : locations) for (const auto* name : names) {
		std::error_code error;
		const auto compiler{location / name};
		if (std::filesystem::is_regular_file(compiler, error) &&
			std::find(found.begin(), found.end(), compiler.string()) == found.end()) found.push_back(compiler.string());
	}
	return found;
}

std::string findNvrtcDll() {
	const auto available{nvrtcLibraries()};
	return available.empty() ? std::string{} : available.front();
}

namespace {

using DevicePtr = unsigned long long;
using Context = void*;
using Module = void*;
using Function = void*;
using Stream = void*;
using NvrtcProgram = void*;
constexpr std::uint32_t kCudaBatchLimit{67108864};
constexpr std::uint32_t kShaWordFloor{0x0147ae14U};

void requireCuda(int result, const char* operation) {
	if (result != 0) {
		throw std::runtime_error(std::string{"CUDA "} + operation +
			" failed with status " + std::to_string(result) + ".");
	}
}

class CudaApi {
public:
	explicit CudaApi(const std::string& nvrtc_dll) {
#if defined(_WIN32)
			driver = DynamicLibrary{"nvcuda.dll", true};
#else
			driver = DynamicLibrary{"libcuda.so.1", true};
#endif
			compiler = DynamicLibrary{nvrtc_dll};

			cuInit = symbol<decltype(cuInit)>(driver, "cuInit");
			cuDriverGetVersion = symbol<decltype(cuDriverGetVersion)>(driver, "cuDriverGetVersion");
			cuDeviceGetUuid = reinterpret_cast<decltype(cuDeviceGetUuid)>(driver.symbol("cuDeviceGetUuid_v2"));
			if (!cuDeviceGetUuid) cuDeviceGetUuid = symbol<decltype(cuDeviceGetUuid)>(driver, "cuDeviceGetUuid");
			cuDeviceGetCount = symbol<decltype(cuDeviceGetCount)>(driver, "cuDeviceGetCount");
			cuDeviceGet = symbol<decltype(cuDeviceGet)>(driver, "cuDeviceGet");
			cuDeviceGetName = symbol<decltype(cuDeviceGetName)>(driver, "cuDeviceGetName");
			cuDeviceGetAttribute = symbol<decltype(cuDeviceGetAttribute)>(driver, "cuDeviceGetAttribute");
			cuCtxCreate = symbol<decltype(cuCtxCreate)>(driver, "cuCtxCreate_v2");
			cuCtxDestroy = symbol<decltype(cuCtxDestroy)>(driver, "cuCtxDestroy_v2");
			cuCtxSetCurrent = symbol<decltype(cuCtxSetCurrent)>(driver, "cuCtxSetCurrent");
			cuStreamCreate = symbol<decltype(cuStreamCreate)>(driver, "cuStreamCreate");
			cuStreamSynchronize = symbol<decltype(cuStreamSynchronize)>(driver, "cuStreamSynchronize");
			cuStreamDestroy = symbol<decltype(cuStreamDestroy)>(driver, "cuStreamDestroy_v2");
			cuMemAlloc = symbol<decltype(cuMemAlloc)>(driver, "cuMemAlloc_v2");
			cuMemFree = symbol<decltype(cuMemFree)>(driver, "cuMemFree_v2");
			cuMemHostAlloc = symbol<decltype(cuMemHostAlloc)>(driver, "cuMemHostAlloc");
			cuMemFreeHost = symbol<decltype(cuMemFreeHost)>(driver, "cuMemFreeHost");
			cuMemcpyHtoDAsync = symbol<decltype(cuMemcpyHtoDAsync)>(driver, "cuMemcpyHtoDAsync_v2");
			cuMemcpyDtoHAsync = symbol<decltype(cuMemcpyDtoHAsync)>(driver, "cuMemcpyDtoHAsync_v2");
			cuMemsetD32Async = symbol<decltype(cuMemsetD32Async)>(driver, "cuMemsetD32Async");
			cuModuleLoadData = symbol<decltype(cuModuleLoadData)>(driver, "cuModuleLoadData");
			cuModuleGetFunction = symbol<decltype(cuModuleGetFunction)>(driver, "cuModuleGetFunction");
			cuModuleUnload = symbol<decltype(cuModuleUnload)>(driver, "cuModuleUnload");
			cuLaunchKernel = symbol<decltype(cuLaunchKernel)>(driver, "cuLaunchKernel");
			nvrtcVersion = symbol<decltype(nvrtcVersion)>(compiler, "nvrtcVersion");
			nvrtcCreateProgram = symbol<decltype(nvrtcCreateProgram)>(compiler, "nvrtcCreateProgram");
			nvrtcCompileProgram = symbol<decltype(nvrtcCompileProgram)>(compiler, "nvrtcCompileProgram");
			nvrtcGetProgramLogSize = symbol<decltype(nvrtcGetProgramLogSize)>(compiler, "nvrtcGetProgramLogSize");
			nvrtcGetProgramLog = symbol<decltype(nvrtcGetProgramLog)>(compiler, "nvrtcGetProgramLog");
			nvrtcGetPTXSize = symbol<decltype(nvrtcGetPTXSize)>(compiler, "nvrtcGetPTXSize");
			nvrtcGetPTX = symbol<decltype(nvrtcGetPTX)>(compiler, "nvrtcGetPTX");
			nvrtcDestroyProgram = symbol<decltype(nvrtcDestroyProgram)>(compiler, "nvrtcDestroyProgram");
			nvrtcGetCUBINSize = reinterpret_cast<decltype(nvrtcGetCUBINSize)>(compiler.symbol("nvrtcGetCUBINSize"));
			nvrtcGetCUBIN = reinterpret_cast<decltype(nvrtcGetCUBIN)>(compiler.symbol("nvrtcGetCUBIN"));
			nvrtcGetNumSupportedArchs = reinterpret_cast<decltype(nvrtcGetNumSupportedArchs)>(compiler.symbol("nvrtcGetNumSupportedArchs"));
			nvrtcGetSupportedArchs = reinterpret_cast<decltype(nvrtcGetSupportedArchs)>(compiler.symbol("nvrtcGetSupportedArchs"));
			int major{}, minor{};
			requireCuda(nvrtcVersion(&major, &minor), "NVRTC version query");
			const auto folder{std::filesystem::path{nvrtc_dll}.parent_path()};
#if defined(_WIN32)
			const auto filename{"nvrtc-builtins64_" + std::to_string(major) + std::to_string(minor) + ".dll"};
#else
			const auto filename{"libnvrtc-builtins.so." + std::to_string(major) + "." + std::to_string(minor)};
#endif
			std::error_code error;
			if (!folder.empty() && std::filesystem::is_regular_file(folder / filename, error))
				builtins = DynamicLibrary{(folder / filename).string()};
	}

	~CudaApi() = default;
	CudaApi(const CudaApi&) = delete;
	CudaApi& operator=(const CudaApi&) = delete;

	int (*cuInit)(unsigned){};
	int (*cuDriverGetVersion)(int*){};
	int (*cuDeviceGetUuid)(void*, int){};
	int (*cuDeviceGetCount)(int*){};
	int (*cuDeviceGet)(int*, int){};
	int (*cuDeviceGetName)(char*, int, int){};
	int (*cuDeviceGetAttribute)(int*, int, int){};
	int (*cuCtxCreate)(Context*, unsigned, int){};
	int (*cuCtxDestroy)(Context){};
	int (*cuCtxSetCurrent)(Context){};
	int (*cuStreamCreate)(Stream*, unsigned){};
	int (*cuStreamSynchronize)(Stream){};
	int (*cuStreamDestroy)(Stream){};
	int (*cuMemAlloc)(DevicePtr*, std::size_t){};
	int (*cuMemFree)(DevicePtr){};
	int (*cuMemHostAlloc)(void**, std::size_t, unsigned){};
	int (*cuMemFreeHost)(void*){};
	int (*cuMemcpyHtoDAsync)(DevicePtr, const void*, std::size_t, Stream){};
	int (*cuMemcpyDtoHAsync)(void*, DevicePtr, std::size_t, Stream){};
	int (*cuMemsetD32Async)(DevicePtr, unsigned, std::size_t, Stream){};
	int (*cuModuleLoadData)(Module*, const void*){};
	int (*cuModuleGetFunction)(Function*, Module, const char*){};
	int (*cuModuleUnload)(Module){};
	int (*cuLaunchKernel)(Function, unsigned, unsigned, unsigned,
		unsigned, unsigned, unsigned, unsigned, Stream, void**, void**){};
	int (*nvrtcVersion)(int*, int*){};
	int (*nvrtcCreateProgram)(NvrtcProgram*, const char*, const char*, int,
		const char* const*, const char* const*){};
	int (*nvrtcCompileProgram)(NvrtcProgram, int, const char* const*){};
	int (*nvrtcGetProgramLogSize)(NvrtcProgram, std::size_t*){};
	int (*nvrtcGetProgramLog)(NvrtcProgram, char*){};
	int (*nvrtcGetPTXSize)(NvrtcProgram, std::size_t*){};
	int (*nvrtcGetPTX)(NvrtcProgram, char*){};
	int (*nvrtcGetCUBINSize)(NvrtcProgram, std::size_t*){};
	int (*nvrtcGetCUBIN)(NvrtcProgram, char*){};
	int (*nvrtcGetNumSupportedArchs)(int*){};
	int (*nvrtcGetSupportedArchs)(int*){};
	int (*nvrtcDestroyProgram)(NvrtcProgram*){};

private:
	template<typename FunctionPointer>
	static FunctionPointer symbol(const DynamicLibrary& library, const char* name) {
		auto result{reinterpret_cast<FunctionPointer>(library.symbol(name))};
		if (!result) throw std::runtime_error(std::string{"CUDA DLL is missing "} + name + ".");
		return result;
	}

	DynamicLibrary driver;
	DynamicLibrary builtins;
	DynamicLibrary compiler;
};

std::vector<char> compileKernel(CudaApi& api, int architecture) {
	int major{}, minor{};
	requireCuda(api.nvrtcVersion(&major, &minor), "NVRTC version query");
	std::string kernel_source;
	for (const auto chunk : kWarthogCudaKernelChunks) kernel_source += chunk;
	NvrtcProgram program{};
	requireCuda(api.nvrtcCreateProgram(&program, kernel_source.c_str(),
		"warthog_sha256t.cu", 0, nullptr, nullptr), "NVRTC program creation");
	struct ProgramGuard {
		CudaApi& api;
		NvrtcProgram& program;
		~ProgramGuard() { if (program) api.nvrtcDestroyProgram(&program); }
	} guard{api, program};
	const bool cubin{api.nvrtcGetCUBINSize && api.nvrtcGetCUBIN};
	const std::string arch_option{std::string{"--gpu-architecture="} + (cubin ? "sm_" : "compute_") +
		std::to_string(architecture)};
	const std::array<const char*, 2> options{"--std=c++14", arch_option.c_str()};
	const int compiled{api.nvrtcCompileProgram(program,
		static_cast<int>(options.size()), options.data())};
	if (compiled != 0) {
		std::size_t log_size{};
		api.nvrtcGetProgramLogSize(program, &log_size);
		std::string log(log_size, '\0');
		if (log_size) api.nvrtcGetProgramLog(program, log.data());
		throw std::runtime_error("NVRTC CUDA kernel compilation failed: " + log);
	}
	std::size_t ptx_size{};
	requireCuda(cubin ? api.nvrtcGetCUBINSize(program, &ptx_size) : api.nvrtcGetPTXSize(program, &ptx_size), "NVRTC output size query");
	std::vector<char> ptx(ptx_size);
	requireCuda(cubin ? api.nvrtcGetCUBIN(program, ptx.data()) : api.nvrtcGetPTX(program, ptx.data()), "NVRTC output read");
	return ptx;
}

std::string cudaDeviceName(CudaApi& api, int device) {
	std::array<char, 256> name{};
	requireCuda(api.cuDeviceGetName(name.data(), static_cast<int>(name.size()), device),
		"device name query");
	return name.data();
}

CudaDeviceIdentity cudaIdentity(CudaApi& api, int device) {
	CudaDeviceIdentity identity;
	identity.name = cudaDeviceName(api, device);
	int major{}, minor{}, compiler_major{}, compiler_minor{};
	requireCuda(api.cuDeviceGetAttribute(&major, 75, device), "compute major query");
	requireCuda(api.cuDeviceGetAttribute(&minor, 76, device), "compute minor query");
	identity.compute_capability = major * 10 + minor;
	requireCuda(api.cuDriverGetVersion(&identity.driver_version), "driver version query");
	requireCuda(api.nvrtcVersion(&compiler_major, &compiler_minor), "compiler version query");
	identity.compiler_version = compiler_major * 1000 + compiler_minor * 10;
	std::array<unsigned char, 16> uuid{};
	requireCuda(api.cuDeviceGetUuid(uuid.data(), device), "device UUID query");
	std::ostringstream text;
	for (const auto byte : uuid) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
	identity.uuid = text.str();
	return identity;
}

}  // namespace

struct CudaSha256tMiner::Impl {
	explicit Impl(const std::string& nvrtc_dll, std::uint32_t launch_block_size)
		: api{nvrtc_dll}, block_size{launch_block_size} {
		if (block_size < 64 || block_size > 512 ||
			(block_size & (block_size - 1)) != 0)
			throw std::invalid_argument("CUDA block size must be 64, 128, 256, or 512.");
	}
	CudaApi api;
	std::uint32_t block_size;
	Context context{};
	std::array<Stream, 2> streams{};
	Module module{};
	Function scan_function{};
	std::array<DevicePtr, 2> device_jobs{};
	std::array<DevicePtr, 2> device_outputs{};
	void* host_job{};
	std::array<void*, 2> host_outputs{};
	std::array<std::size_t, 2> output_capacities{};
	std::string name;
	CudaDeviceIdentity identity;

	~Impl() {
		if (context) api.cuCtxSetCurrent(context);
		for (auto stream : streams) if (stream) api.cuStreamSynchronize(stream);
		for (auto output : host_outputs) if (output) api.cuMemFreeHost(output);
		if (host_job) api.cuMemFreeHost(host_job);
		for (auto output : device_outputs) if (output) api.cuMemFree(output);
		for (auto job : device_jobs) if (job) api.cuMemFree(job);
		if (module) api.cuModuleUnload(module);
		for (auto stream : streams) if (stream) api.cuStreamDestroy(stream);
		if (context) api.cuCtxDestroy(context);
	}

	void initialize(std::size_t index) {
		requireCuda(api.cuInit(0), "driver initialization");
		int count{};
		requireCuda(api.cuDeviceGetCount(&count), "device count query");
		if (index >= static_cast<std::size_t>(count)) {
			throw std::runtime_error("Selected NVIDIA CUDA device is unavailable.");
		}
		int device{};
		requireCuda(api.cuDeviceGet(&device, static_cast<int>(index)), "device selection");
		name = cudaDeviceName(api, device);
		int major{}, minor{};
		requireCuda(api.cuDeviceGetAttribute(&major, 75, device), "compute major query");
		requireCuda(api.cuDeviceGetAttribute(&minor, 76, device), "compute minor query");
		identity = cudaIdentity(api, device);
		// CU_CTX_SCHED_BLOCKING_SYNC lets the host sleep during kernel execution.
		requireCuda(api.cuCtxCreate(&context, 4, device), "blocking context creation");
		for (auto& stream : streams)
			requireCuda(api.cuStreamCreate(&stream, 1), "stream creation");
		const auto ptx{compileKernel(api, major * 10 + minor)};
		requireCuda(api.cuModuleLoadData(&module, ptx.data()), "PTX module load");
		requireCuda(api.cuModuleGetFunction(&scan_function, module, "scan"),
			"SHA256t kernel lookup");
		for (auto& job : device_jobs)
			requireCuda(api.cuMemAlloc(&job, 84), "job buffer allocation");
		requireCuda(api.cuMemHostAlloc(&host_job, 84, 0), "pinned job allocation");
	}

	void ensureCapacity(std::size_t index, std::size_t needed) {
		if (needed <= output_capacities[index]) return;
		const auto grown{std::min<std::size_t>(kCudaBatchLimit,
			std::max(needed, output_capacities[index] ? output_capacities[index] * 2 : 4096))};
		if (host_outputs[index]) {
			api.cuMemFreeHost(host_outputs[index]); host_outputs[index] = nullptr;
		}
		if (device_outputs[index]) {
			api.cuMemFree(device_outputs[index]); device_outputs[index] = 0;
		}
		const auto bytes{4 + grown * sizeof(Sha256tCandidate)};
		requireCuda(api.cuMemAlloc(&device_outputs[index], bytes), "candidate buffer allocation");
		requireCuda(api.cuMemHostAlloc(&host_outputs[index], bytes, 0), "pinned candidate allocation");
		output_capacities[index] = grown;
	}

	std::vector<Sha256tCandidate> scan(const Header& header,
		std::uint32_t nonce_start, std::uint32_t count, std::uint32_t threshold) {
		requireCuda(api.cuCtxSetCurrent(context), "context selection");
		if (count == 0 || count > kCudaBatchLimit ||
			static_cast<std::uint64_t>(nonce_start) + count >
			static_cast<std::uint64_t>(UINT32_MAX) + 1) {
			throw std::invalid_argument("CUDA Warthog nonce batch is out of range.");
		}
		std::array<std::uint32_t, 21> job{};
		const auto midstate{warthogSha256Midstate(header)};
		std::copy(midstate.begin(), midstate.end(), job.begin());
		for (std::size_t i{}; i < 3; ++i) {
			const auto p{64 + i * 4};
			job[8 + i] = (static_cast<std::uint32_t>(header[p]) << 24) |
				(static_cast<std::uint32_t>(header[p + 1]) << 16) |
				(static_cast<std::uint32_t>(header[p + 2]) << 8) | header[p + 3];
		}
		// The first three rounds of the header tail depend only on job words.
		// Preserve the original midstate for SHA's final feed-forward.
		std::uint32_t a{job[0]}, b{job[1]}, c{job[2]}, d{job[3]};
		std::uint32_t e{job[4]}, f{job[5]}, g{job[6]}, h{job[7]};
		constexpr std::array<std::uint32_t, 3> round_constants{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU};
		for (std::size_t i{}; i < round_constants.size(); ++i) {
			const std::uint32_t sigma1{std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)};
			const std::uint32_t sigma0{std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)};
			const std::uint32_t choice{(e & f) ^ (~e & g)};
			const std::uint32_t majority{(a & b) ^ (a & c) ^ (b & c)};
			const std::uint32_t t1{h + sigma1 + choice + round_constants[i] + job[8 + i]};
			const std::uint32_t t2{sigma0 + majority};
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		// Physical SHA registers after three argument-rotated kernel rounds.
		job[11] = e; job[12] = f; job[13] = g;
		job[14] = a; job[15] = b; job[16] = c;
		const auto sigma1 = [](std::uint32_t value) {
			return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
		};
		const auto sigma0 = [](std::uint32_t value) {
			return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
		};
		const auto small0 = [](std::uint32_t value) {
			return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3);
		};
		const auto small1 = [](std::uint32_t value) {
			return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10);
		};
		job[17] = h + sigma1(e) + ((e & f) ^ (~e & g)) + 0xe9b5dba5U;
		job[18] = sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
		job[19] = job[8] + small0(job[9]);
		job[20] = job[9] + small1(640U) + small0(job[10]);
		std::memcpy(host_job, job.data(), sizeof(job));
		if (threshold <= kShaWordFloor) return {};
		std::uint32_t lo{kShaWordFloor};
		std::uint32_t hi{threshold - 1};
		const double fraction{threshold > lo
			? static_cast<double>(threshold - lo) / 4294967296.0 : 0.0};
		const std::array<std::uint32_t, 2> part_counts{count, 0};
		const std::array<std::uint32_t, 2> part_starts{nonce_start,
			static_cast<std::uint32_t>(nonce_start + part_counts[0])};
		std::array<std::size_t, 2> requested{};
		for (std::size_t i{}; i < 2; ++i) {
			if (part_counts[i]) requested[i] = std::min<std::size_t>(part_counts[i],
				std::max<std::size_t>(4096, static_cast<std::size_t>(
					std::ceil(static_cast<double>(part_counts[i]) * fraction * 1.25 + 4096.0))));
		}
		for (;;) {
			for (std::size_t i{}; i < 2; ++i) {
				if (!part_counts[i]) continue;
				ensureCapacity(i, requested[i]);
				requireCuda(api.cuMemcpyHtoDAsync(device_jobs[i], host_job, 84, streams[i]),
					"job upload");
				requireCuda(api.cuMemsetD32Async(device_outputs[i], 0, 1, streams[i]),
					"candidate counter reset");
				DevicePtr job_arg{device_jobs[i]}, output_arg{device_outputs[i]};
				std::uint32_t capacity_arg{static_cast<std::uint32_t>(requested[i])};
				auto start_arg{part_starts[i]}, count_arg{part_counts[i]};
				void* arguments[]{&job_arg, &start_arg, &count_arg, &lo,
					&hi, &capacity_arg, &output_arg};
				const auto blocks{(part_counts[i] + block_size - 1) / block_size};
				requireCuda(api.cuLaunchKernel(scan_function, blocks, 1, 1,
					block_size, 1, 1, 0, streams[i], arguments, nullptr),
					"SHA256t kernel launch");
				const auto bytes{4 + requested[i] * sizeof(Sha256tCandidate)};
				requireCuda(api.cuMemcpyDtoHAsync(host_outputs[i], device_outputs[i],
					bytes, streams[i]), "candidate readback");
			}
			std::array<std::uint32_t, 2> actual{};
			bool overflow{};
			for (std::size_t i{}; i < 2; ++i) {
				if (!part_counts[i]) continue;
				requireCuda(api.cuStreamSynchronize(streams[i]), "scan completion");
				std::memcpy(&actual[i], host_outputs[i], sizeof(actual[i]));
				if (actual[i] > requested[i]) {
					overflow = true;
					requested[i] = std::min<std::size_t>(part_counts[i],
						std::max<std::size_t>(actual[i], requested[i] * 2));
				}
			}
			if (overflow) continue;
			std::vector<Sha256tCandidate> candidates;
			candidates.reserve(static_cast<std::size_t>(actual[0]) + actual[1]);
			for (std::size_t i{}; i < 2; ++i) {
				if (!actual[i]) continue;
				const auto* first{reinterpret_cast<const Sha256tCandidate*>(
					static_cast<const std::uint8_t*>(host_outputs[i]) + 4)};
				candidates.insert(candidates.end(), first, first + actual[i]);
			}
			return candidates;
		}
	}
};

std::vector<CudaDeviceIdentity> CudaSha256tMiner::identities(const std::string& nvrtc_dll) {
	CudaApi api{nvrtc_dll};
	requireCuda(api.cuInit(0), "driver initialization");
	int count{};
	requireCuda(api.cuDeviceGetCount(&count), "device enumeration");
	std::vector<CudaDeviceIdentity> names;
	std::vector<int> supported;
	if (api.nvrtcGetNumSupportedArchs && api.nvrtcGetSupportedArchs) {
		int size{};
		requireCuda(api.nvrtcGetNumSupportedArchs(&size), "supported architecture count");
		if (size <= 0 || size > 4096) throw std::runtime_error("Invalid NVRTC architecture list.");
		supported.resize(size);
		requireCuda(api.nvrtcGetSupportedArchs(supported.data()), "supported architecture list");
	}
	for (int i{}; i < count; ++i) {
		int device{};
		requireCuda(api.cuDeviceGet(&device, i), "device selection");
		int major{}, minor{};
		requireCuda(api.cuDeviceGetAttribute(&major, 75, device), "compute major query");
		requireCuda(api.cuDeviceGetAttribute(&minor, 76, device), "compute minor query");
		if (!supported.empty() && std::find(supported.begin(), supported.end(), major * 10 + minor) == supported.end())
			throw std::runtime_error("NVRTC compiler does not support GPU compute capability " + std::to_string(major * 10 + minor));
		auto identity{cudaIdentity(api, device)};
		if (identity.driver_version / 1000 < identity.compiler_version / 1000)
			throw std::runtime_error("CUDA compiler major version requires a newer NVIDIA driver.");
		names.push_back(std::move(identity));
	}
	return names;
}

std::vector<std::string> CudaSha256tMiner::devices(const std::string& library) {
	std::vector<std::string> names;
	for (const auto& identity : identities(library)) names.push_back(identity.name);
	return names;
}

CudaSha256tMiner::CudaSha256tMiner(std::size_t device_index,
	const std::string& nvrtc_dll, std::uint32_t block_size)
	: impl{std::make_unique<Impl>(nvrtc_dll, block_size)} {
	impl->initialize(device_index);
}

CudaSha256tMiner::~CudaSha256tMiner() = default;

std::vector<Sha256tCandidate> CudaSha256tMiner::scan(const Header& header,
	std::uint32_t nonce_start, std::uint32_t nonce_count,
	std::uint32_t candidate_threshold) {
	return impl->scan(header, nonce_start, nonce_count, candidate_threshold);
}

const std::string& CudaSha256tMiner::deviceName() const { return impl->name; }
const CudaDeviceIdentity& CudaSha256tMiner::identity() const { return impl->identity; }

void CudaSha256tMiner::setBlockSize(std::uint32_t value) {
	if (value < 64 || value > 512 || (value & (value - 1)))
		throw std::invalid_argument("CUDA block size must be 64, 128, 256, or 512.");
	impl->block_size = value;
}

CudaLinkBandwidth CudaSha256tMiner::measureBandwidth(std::size_t bytes) {
	if (bytes < 1048576 || bytes > 268435456) throw std::invalid_argument("Bandwidth probe size must be 1..256 MiB.");
	auto& api{impl->api};
	requireCuda(api.cuCtxSetCurrent(impl->context), "bandwidth context selection");
	struct Buffers {
		CudaApi& api;
		DevicePtr device{};
		void* host{};
		~Buffers() { if (device) api.cuMemFree(device); if (host) api.cuMemFreeHost(host); }
	} buffers{api};
	requireCuda(api.cuMemAlloc(&buffers.device, bytes), "bandwidth device allocation");
	requireCuda(api.cuMemHostAlloc(&buffers.host, bytes, 0), "bandwidth pinned allocation");
	std::memset(buffers.host, 0x5a, bytes);
	const auto transfer = [&](bool upload) {
		if (upload) requireCuda(api.cuMemcpyHtoDAsync(buffers.device, buffers.host, bytes, impl->streams[0]), "bandwidth upload");
		else requireCuda(api.cuMemcpyDtoHAsync(buffers.host, buffers.device, bytes, impl->streams[0]), "bandwidth download");
		requireCuda(api.cuStreamSynchronize(impl->streams[0]), "bandwidth synchronization");
	};
	transfer(true);
	const auto measure = [&](bool upload) {
		const auto start{std::chrono::steady_clock::now()};
		for (unsigned i{}; i < 3; ++i) transfer(upload);
		return 3.0 * static_cast<double>(bytes) / std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	};
	const double upload{measure(true)}, download{measure(false)};
	if (static_cast<unsigned char*>(buffers.host)[0] != 0x5a || static_cast<unsigned char*>(buffers.host)[bytes - 1] != 0x5a)
		throw std::runtime_error("Bandwidth probe memory verification failed.");
	return {upload, download};
}

}  // namespace brminer::warthog
