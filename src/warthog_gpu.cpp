#include "warthog_gpu.hpp"

#include "warthog_gpu_kernel.hpp"
#include "platform.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

namespace brminer::warthog {

namespace {

constexpr cl_uint kNonceBatchSize{4194304};
// Janus uses the leading SHA256t word for every candidate above the floor.
// Full SHA256t is recomputed on the CPU before submitting a potential share.
constexpr std::size_t kWordsPerCandidate{2};
constexpr cl_int kPlatformNotFound{-1001};

std::array<std::uint32_t, 8> sha256Midstate(const Header& header) {
	static constexpr std::array<std::uint32_t, 64> rounds{
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	std::array<std::uint32_t, 64> words{};
	for (std::size_t i{}; i < 16; ++i) {
		const std::size_t p{i * 4};
		words[i] = (static_cast<std::uint32_t>(header[p]) << 24) |
			(static_cast<std::uint32_t>(header[p + 1]) << 16) |
			(static_cast<std::uint32_t>(header[p + 2]) << 8) | header[p + 3];
	}
	for (std::size_t i{16}; i < words.size(); ++i) {
		const auto a{words[i - 15]};
		const auto b{words[i - 2]};
		words[i] = words[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) +
			words[i - 7] + (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
	}
	auto a{state[0]}, b{state[1]}, c{state[2]}, d{state[3]};
	auto e{state[4]}, f{state[5]}, g{state[6]}, h{state[7]};
	for (std::size_t i{}; i < words.size(); ++i) {
		const auto t1{h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
			((e & f) ^ (~e & g)) + rounds[i] + words[i]};
		const auto t2{(std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
			((a & b) ^ (a & c) ^ (b & c))};
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
	return state;
}

void requireCl(cl_int result, const char* operation) {
	if (result != CL_SUCCESS) {
		throw std::runtime_error(std::string{"OpenCL "} + operation + " failed with error " +
							 std::to_string(result) + ".");
	}
}

class OpenClApi {
public:
	OpenClApi() {
		try {
#if defined(_WIN32)
			library = DynamicLibrary{"OpenCL.dll", true};
#else
			library = DynamicLibrary{"libOpenCL.so.1", true};
#endif
		} catch (const std::runtime_error&) { return; }
		get_platform_ids = load<decltype(get_platform_ids)>("clGetPlatformIDs");
		get_device_ids = load<decltype(get_device_ids)>("clGetDeviceIDs");
		get_device_info = load<decltype(get_device_info)>("clGetDeviceInfo");
		create_context = load<decltype(create_context)>("clCreateContext");
		create_command_queue = load<decltype(create_command_queue)>("clCreateCommandQueue");
		create_program_with_source = load<decltype(create_program_with_source)>("clCreateProgramWithSource");
		build_program = load<decltype(build_program)>("clBuildProgram");
		get_program_build_info = load<decltype(get_program_build_info)>("clGetProgramBuildInfo");
		create_kernel = load<decltype(create_kernel)>("clCreateKernel");
		create_buffer = load<decltype(create_buffer)>("clCreateBuffer");
		set_kernel_arg = load<decltype(set_kernel_arg)>("clSetKernelArg");
		enqueue_write_buffer = load<decltype(enqueue_write_buffer)>("clEnqueueWriteBuffer");
		enqueue_nd_range_kernel = load<decltype(enqueue_nd_range_kernel)>("clEnqueueNDRangeKernel");
		enqueue_read_buffer = load<decltype(enqueue_read_buffer)>("clEnqueueReadBuffer");
		finish = load<decltype(finish)>("clFinish");
		release_mem_object = load<decltype(release_mem_object)>("clReleaseMemObject");
		release_kernel = load<decltype(release_kernel)>("clReleaseKernel");
		release_program = load<decltype(release_program)>("clReleaseProgram");
		release_command_queue = load<decltype(release_command_queue)>("clReleaseCommandQueue");
		release_context = load<decltype(release_context)>("clReleaseContext");
	}

	~OpenClApi() = default;
	OpenClApi(const OpenClApi&) = delete;
	OpenClApi& operator=(const OpenClApi&) = delete;

	explicit operator bool() const { return static_cast<bool>(library); }

	decltype(&clGetPlatformIDs) get_platform_ids{};
	decltype(&clGetDeviceIDs) get_device_ids{};
	decltype(&clGetDeviceInfo) get_device_info{};
	decltype(&clCreateContext) create_context{};
	decltype(&clCreateCommandQueue) create_command_queue{};
	decltype(&clCreateProgramWithSource) create_program_with_source{};
	decltype(&clBuildProgram) build_program{};
	decltype(&clGetProgramBuildInfo) get_program_build_info{};
	decltype(&clCreateKernel) create_kernel{};
	decltype(&clCreateBuffer) create_buffer{};
	decltype(&clSetKernelArg) set_kernel_arg{};
	decltype(&clEnqueueWriteBuffer) enqueue_write_buffer{};
	decltype(&clEnqueueNDRangeKernel) enqueue_nd_range_kernel{};
	decltype(&clEnqueueReadBuffer) enqueue_read_buffer{};
	decltype(&clFinish) finish{};
	decltype(&clReleaseMemObject) release_mem_object{};
	decltype(&clReleaseKernel) release_kernel{};
	decltype(&clReleaseProgram) release_program{};
	decltype(&clReleaseCommandQueue) release_command_queue{};
	decltype(&clReleaseContext) release_context{};

private:
	template <typename Function>
	Function load(const char* name) {
		auto function{reinterpret_cast<Function>(library.symbol(name))};
		if (!function) {
			throw std::runtime_error(std::string{"OpenCL runtime is missing required function "} + name + ".");
		}
		return function;
	}

	DynamicLibrary library;
};

struct DeviceInfo {
	cl_device_id device{};
	std::string name;
};

std::string deviceString(OpenClApi& api, cl_device_id device, cl_device_info parameter) {
	size_t size{};
	requireCl(api.get_device_info(device, parameter, 0, nullptr, &size), "device information query");
	std::string value(size, '\0');
	requireCl(api.get_device_info(device, parameter, size, value.data(), nullptr), "device information read");
	if (!value.empty() && value.back() == '\0') value.pop_back();
	return value;
}

std::vector<DeviceInfo> enumerateDevices(OpenClApi& api) {
	if (!api) return {};
	cl_uint platform_count{};
	const cl_int platform_result{api.get_platform_ids(0, nullptr, &platform_count)};
	if (platform_result == kPlatformNotFound || (platform_result == CL_SUCCESS && platform_count == 0)) return {};
	requireCl(platform_result, "platform enumeration");
	std::vector<cl_platform_id> platforms(platform_count);
	requireCl(api.get_platform_ids(platform_count, platforms.data(), nullptr), "platform enumeration");

	std::vector<DeviceInfo> devices;
	for (const cl_platform_id platform : platforms) {
		cl_uint device_count{};
		const cl_int device_result{api.get_device_ids(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &device_count)};
		if (device_result == CL_DEVICE_NOT_FOUND || (device_result == CL_SUCCESS && device_count == 0)) continue;
		requireCl(device_result, "GPU enumeration");
		std::vector<cl_device_id> platform_devices(device_count);
		requireCl(api.get_device_ids(platform, CL_DEVICE_TYPE_GPU, device_count, platform_devices.data(), nullptr),
				  "GPU enumeration");
		for (const cl_device_id device : platform_devices) {
			devices.push_back({device, deviceString(api, device, CL_DEVICE_NAME)});
		}
	}
	return devices;
}

}  // namespace

std::array<std::uint32_t, 8> warthogSha256Midstate(const Header& header) {
	return sha256Midstate(header);
}

struct OpenClSha256tMiner::Impl {
	OpenClApi api;
	cl_context context{};
	cl_command_queue queue{};
	cl_program program{};
	cl_kernel kernel{};
	cl_mem header_buffer{};
	cl_mem output_buffer{};
	Header last_header{};
	bool has_last_header{};
	std::string name;

	~Impl() {
		if (output_buffer) api.release_mem_object(output_buffer);
		if (header_buffer) api.release_mem_object(header_buffer);
		if (kernel) api.release_kernel(kernel);
		if (program) api.release_program(program);
		if (queue) api.release_command_queue(queue);
		if (context) api.release_context(context);
	}
};

std::vector<std::string> OpenClSha256tMiner::devices() {
	OpenClApi api;
	const auto found{enumerateDevices(api)};
	std::vector<std::string> names;
	names.reserve(found.size());
	for (const auto& device : found) names.push_back(device.name);
	return names;
}

OpenClSha256tMiner::OpenClSha256tMiner(std::size_t device_index) : impl{std::make_unique<Impl>()} {
	const auto found{enumerateDevices(impl->api)};
	if (device_index >= found.size()) throw std::runtime_error("Selected OpenCL GPU is no longer available.");
	const auto& selected{found[device_index]};
	impl->name = selected.name;
	cl_int error{};
	impl->context = impl->api.create_context(nullptr, 1, &selected.device, nullptr, nullptr, &error);
	requireCl(error, "context creation");
	impl->queue = impl->api.create_command_queue(impl->context, selected.device, 0, &error);
	requireCl(error, "command queue creation");
	constexpr std::size_t source_count{sizeof(kWarthogOpenClKernelChunks) /
									  sizeof(kWarthogOpenClKernelChunks[0])};
	std::array<const char*, source_count> sources{};
	std::array<size_t, source_count> source_sizes{};
	for (std::size_t i{}; i < source_count; ++i) {
		sources[i] = kWarthogOpenClKernelChunks[i].data();
		source_sizes[i] = kWarthogOpenClKernelChunks[i].size();
	}
	impl->program = impl->api.create_program_with_source(impl->context,
		static_cast<cl_uint>(source_count), sources.data(), source_sizes.data(), &error);
	requireCl(error, "program creation");	error = impl->api.build_program(impl->program, 1, &selected.device,
									"-cl-std=CL1.2 -DC_CONSTANT=0x147ae14", nullptr, nullptr);
	if (error != CL_SUCCESS) {
		size_t log_size{};
		impl->api.get_program_build_info(impl->program, selected.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
		std::string log(log_size, '\0');
		if (log_size != 0) {
			impl->api.get_program_build_info(impl->program, selected.device, CL_PROGRAM_BUILD_LOG,
										 log_size, log.data(), nullptr);
		}
		throw std::runtime_error("OpenCL could not compile the Warthog SHA256t kernel: " + log);
	}
	impl->kernel = impl->api.create_kernel(impl->program, "h", &error);
	requireCl(error, "kernel creation");
	impl->header_buffer = impl->api.create_buffer(impl->context, CL_MEM_READ_ONLY,
		sizeof(Header) + sizeof(std::array<std::uint32_t, 8>), nullptr, &error);
	requireCl(error, "header buffer creation");
	const size_t output_bytes{(1 + static_cast<size_t>(kNonceBatchSize) * kWordsPerCandidate) * sizeof(cl_uint)};
	impl->output_buffer = impl->api.create_buffer(impl->context, CL_MEM_READ_WRITE, output_bytes, nullptr, &error);
	requireCl(error, "result buffer creation");
}

OpenClSha256tMiner::~OpenClSha256tMiner() = default;

const std::string& OpenClSha256tMiner::deviceName() const {
	return impl->name;
}

std::vector<Sha256tCandidate> OpenClSha256tMiner::scan(const Header& header, std::uint32_t nonce_start,
									   std::uint32_t nonce_count,
									   std::uint32_t candidate_threshold) {
	if (nonce_count == 0 || nonce_count > kNonceBatchSize) {
		throw std::invalid_argument("OpenCL Warthog scan batch must contain 1 to 4194304 nonces.");
	}
	cl_uint threshold{candidate_threshold};
	cl_uint maximum_candidates{nonce_count};
	const cl_uint zero{};
	cl_int error{};
	if (!impl->has_last_header || impl->last_header != header) {
		std::array<std::uint8_t, sizeof(Header) + sizeof(std::array<std::uint32_t, 8>)> upload{};
		std::copy(header.begin(), header.end(), upload.begin());
		const auto midstate{sha256Midstate(header)};
		std::memcpy(upload.data() + sizeof(Header), midstate.data(), sizeof(midstate));
		error = impl->api.enqueue_write_buffer(impl->queue, impl->header_buffer, CL_TRUE, 0,
										   upload.size(), upload.data(), 0, nullptr, nullptr);
		requireCl(error, "header upload");
		impl->last_header = header;
		impl->has_last_header = true;
	}
	error = impl->api.enqueue_write_buffer(impl->queue, impl->output_buffer, CL_FALSE, 0,
											   sizeof(zero), &zero, 0, nullptr, nullptr);
	requireCl(error, "result counter reset");
	requireCl(impl->api.set_kernel_arg(impl->kernel, 0, sizeof(impl->header_buffer), &impl->header_buffer), "header argument setup");
	requireCl(impl->api.set_kernel_arg(impl->kernel, 1, sizeof(threshold), &threshold), "threshold argument setup");
	requireCl(impl->api.set_kernel_arg(impl->kernel, 2, sizeof(maximum_candidates), &maximum_candidates), "candidate limit setup");
	requireCl(impl->api.set_kernel_arg(impl->kernel, 3, sizeof(impl->output_buffer), &impl->output_buffer), "result argument setup");
	const size_t global_offset{nonce_start};
	const size_t global_size{nonce_count};
	error = impl->api.enqueue_nd_range_kernel(impl->queue, impl->kernel, 1, &global_offset,
										  &global_size, nullptr, 0, nullptr, nullptr);
	requireCl(error, "kernel dispatch");
	cl_uint result_count{};
	requireCl(impl->api.enqueue_read_buffer(impl->queue, impl->output_buffer, CL_TRUE, 0,
											 sizeof(result_count), &result_count, 0, nullptr, nullptr), "result count read");
	if (result_count > nonce_count) throw std::runtime_error("OpenCL Warthog kernel returned too many candidates.");
	static_assert(sizeof(Sha256tCandidate) == kWordsPerCandidate * sizeof(cl_uint));
	static_assert(std::is_trivially_copyable_v<Sha256tCandidate>);
	std::vector<Sha256tCandidate> candidates(result_count);
	if (result_count != 0) {
		requireCl(impl->api.enqueue_read_buffer(impl->queue, impl->output_buffer, CL_TRUE, sizeof(cl_uint),
														 candidates.size() * sizeof(Sha256tCandidate), candidates.data(),
														  0, nullptr, nullptr), "candidate read");
	}
	return candidates;
}

}  // namespace brminer::warthog
