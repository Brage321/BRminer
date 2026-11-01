#include "warthog_gpu.hpp"

#include "warthog_gpu_kernel.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define CL_TARGET_OPENCL_VERSION 120
#include <windows.h>
#include <CL/cl.h>
#endif

namespace brminer::warthog {

#if defined(_WIN32)
namespace {

constexpr cl_uint kNonceBatchSize{65536};
constexpr std::size_t kWordsPerCandidate{9};
constexpr cl_int kPlatformNotFound{-1001};

void requireCl(cl_int result, const char* operation) {
	if (result != CL_SUCCESS) {
		throw std::runtime_error(std::string{"OpenCL "} + operation + " failed with error " +
							 std::to_string(result) + ".");
	}
}

class OpenClApi {
public:
	OpenClApi() : library{LoadLibraryW(L"OpenCL.dll")} {
		if (!library) return;
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

	~OpenClApi() {
		if (library) FreeLibrary(library);
	}
	OpenClApi(const OpenClApi&) = delete;
	OpenClApi& operator=(const OpenClApi&) = delete;

	explicit operator bool() const { return library != nullptr; }

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
		auto function{reinterpret_cast<Function>(GetProcAddress(library, name))};
		if (!function) {
			throw std::runtime_error(std::string{"OpenCL runtime is missing required function "} + name + ".");
		}
		return function;
	}

	HMODULE library{};
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

struct OpenClSha256tMiner::Impl {
	OpenClApi api;
	cl_context context{};
	cl_command_queue queue{};
	cl_program program{};
	cl_kernel kernel{};
	cl_mem header_buffer{};
	cl_mem output_buffer{};
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
	const char* source{kWarthogOpenClKernel};
	const size_t source_size{std::char_traits<char>::length(source)};
	impl->program = impl->api.create_program_with_source(impl->context, 1, &source, &source_size, &error);
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
	impl->header_buffer = impl->api.create_buffer(impl->context, CL_MEM_READ_ONLY, sizeof(Header), nullptr, &error);
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
		throw std::invalid_argument("OpenCL Warthog scan batch must contain 1 to 65536 nonces.");
	}
	cl_uint threshold{candidate_threshold};
	cl_uint maximum_candidates{nonce_count};
	const cl_uint zero{};
	cl_int error{impl->api.enqueue_write_buffer(impl->queue, impl->header_buffer, CL_TRUE, 0,
											   sizeof(header), header.data(), 0, nullptr, nullptr)};
	requireCl(error, "header upload");
	error = impl->api.enqueue_write_buffer(impl->queue, impl->output_buffer, CL_TRUE, 0,
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
	requireCl(impl->api.finish(impl->queue), "kernel completion");
	cl_uint result_count{};
	requireCl(impl->api.enqueue_read_buffer(impl->queue, impl->output_buffer, CL_TRUE, 0,
											 sizeof(result_count), &result_count, 0, nullptr, nullptr), "result count read");
	if (result_count > nonce_count) throw std::runtime_error("OpenCL Warthog kernel returned too many candidates.");
	std::vector<cl_uint> words(1 + static_cast<size_t>(result_count) * kWordsPerCandidate);
	if (result_count != 0) {
		requireCl(impl->api.enqueue_read_buffer(impl->queue, impl->output_buffer, CL_TRUE, sizeof(cl_uint),
												 words.size() * sizeof(cl_uint) - sizeof(cl_uint), words.data() + 1,
												 0, nullptr, nullptr), "candidate read");
	}
	std::vector<Sha256tCandidate> candidates;
	candidates.reserve(result_count);
	for (cl_uint i{}; i < result_count; ++i) {
		const std::size_t offset{1 + static_cast<std::size_t>(i) * kWordsPerCandidate};
		Sha256tCandidate candidate;
		candidate.nonce = words[offset];
		for (std::size_t word{}; word < 8; ++word) {
			const cl_uint value{words[offset + 1 + word]};
			candidate.hash[word * 4] = static_cast<std::uint8_t>(value >> 24);
			candidate.hash[word * 4 + 1] = static_cast<std::uint8_t>(value >> 16);
			candidate.hash[word * 4 + 2] = static_cast<std::uint8_t>(value >> 8);
			candidate.hash[word * 4 + 3] = static_cast<std::uint8_t>(value);
		}
		candidates.push_back(std::move(candidate));
	}
	return candidates;
}

#else

struct OpenClSha256tMiner::Impl {};

std::vector<std::string> OpenClSha256tMiner::devices() {
	return {};
}

OpenClSha256tMiner::OpenClSha256tMiner(std::size_t) {
	throw std::runtime_error("Warthog GPU mining requires Windows OpenCL support.");
}

OpenClSha256tMiner::~OpenClSha256tMiner() = default;

const std::string& OpenClSha256tMiner::deviceName() const {
	static const std::string unavailable;
	return unavailable;
}

std::vector<Sha256tCandidate> OpenClSha256tMiner::scan(const Header&, std::uint32_t, std::uint32_t,
													 std::uint32_t) {
	throw std::runtime_error("Warthog GPU mining requires Windows OpenCL support.");
}

#endif

}  // namespace brminer::warthog
