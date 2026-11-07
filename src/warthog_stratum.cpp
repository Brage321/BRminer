#include "warthog_miner.hpp"

#include "bounded_mpmc_ring.hpp"
#include "config.hpp"
#include "platform.hpp"
#include "warthog_cuda.hpp"
#include "warthog_console.hpp"
#include "warthog_gpu.hpp"
#include "warthog_pow.hpp"
#include "warthog_benchmark.hpp"
#include "calibration.hpp"

#include <crypto/sha256.h>
#include <univalue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <charconv>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SOCKET = int;
constexpr int INVALID_SOCKET{-1};
constexpr int SOCKET_ERROR{-1};
constexpr int WSAETIMEDOUT{ETIMEDOUT};
constexpr int WSAEWOULDBLOCK{EAGAIN};
inline int WSAGetLastError() { return errno; }
inline int closesocket(int socket) { return ::close(socket); }
#endif

namespace brminer::warthog {
namespace {

constexpr std::uint32_t kSubscribeId = 1;
constexpr std::uint32_t kAuthorizeId = 2;
constexpr std::uint32_t kSha256tCandidateFloor{0x0147ae14U};
constexpr double kInitialGpuCandidateFraction{0.00005};
constexpr double kTargetShareIntervalSeconds{60.0};
constexpr std::size_t kGpuCandidateQueueLimit{4194304};
constexpr std::size_t kGpuCandidateTakeBatch{1024};

std::uint32_t candidateThreshold(double fraction) {
	const auto range{static_cast<std::uint64_t>(UINT32_MAX) - kSha256tCandidateFloor};
	return kSha256tCandidateFloor + static_cast<std::uint32_t>(fraction * range);
}

volatile std::sig_atomic_t g_stop_requested{0};

void requestStop(int) {
	g_stop_requested = 1;
}

std::string processorDescription() {
#if defined(_WIN32)
	std::array<char, 256> value{};
	DWORD value_size{static_cast<DWORD>(value.size())};
	const LSTATUS result{RegGetValueA(
		HKEY_LOCAL_MACHINE,
		"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
		"ProcessorNameString", RRF_RT_REG_SZ, nullptr, value.data(), &value_size)};
	if (result == ERROR_SUCCESS) {
		std::string description{value.data()};
		const auto last_character{description.find_last_not_of(' ')};
		if (last_character != std::string::npos) description.resize(last_character + 1);
		if (!description.empty()) return description;
	}
#endif
	return "Unknown CPU";
}

struct ProcessorTarget {
	std::uint16_t group{};
	std::uint16_t number{};
};

struct AffinityPlan {
	std::string policy{"none"};
	std::vector<std::optional<ProcessorTarget>> cpu_workers;
	std::vector<std::optional<ProcessorTarget>> gpu_workers;
	std::vector<std::string> assignments;
};

struct FilterPolicy {
	std::string name{"current"};
	std::chrono::milliseconds adjustment_interval{500};
	double drain_horizon_seconds{2.0};
	double queue_target_fraction{0.5};
};

FilterPolicy parseFilterPolicy(std::string value) {
	if (value.empty() || value == "current") return {};
	unsigned interval_ms{}, horizon_seconds{}, target_percent{};
	char trailing{};
	if (std::sscanf(value.c_str(), "i%u-h%u-t%u%c", &interval_ms,
		&horizon_seconds, &target_percent, &trailing) != 3 ||
		(interval_ms != 250 && interval_ms != 500 && interval_ms != 1000) ||
		(horizon_seconds != 1 && horizon_seconds != 2 && horizon_seconds != 4) ||
		(target_percent != 25 && target_percent != 50 && target_percent != 75)) {
		throw std::invalid_argument(
			"BRMINER_EXPERIMENT_FILTER must be current or i{250,500,1000}-h{1,2,4}-t{25,50,75}.");
	}
	return {std::move(value), std::chrono::milliseconds{interval_ms},
		static_cast<double>(horizon_seconds), static_cast<double>(target_percent) / 100.0};
}

std::string processorTargetName(const ProcessorTarget& target) {
	return "g" + std::to_string(target.group) + ":p" + std::to_string(target.number);
}

#if defined(_WIN32)
std::vector<std::vector<ProcessorTarget>> processorCoreTopology() {
	DWORD bytes{};
	if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes) ||
		GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
		throw std::runtime_error("Could not query Windows processor topology (size): " +
			std::to_string(GetLastError()));
	}
	std::vector<std::uint8_t> storage(bytes);
	auto* information{reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data())};
	if (!GetLogicalProcessorInformationEx(RelationProcessorCore, information, &bytes)) {
		throw std::runtime_error("Could not query Windows processor topology: " +
			std::to_string(GetLastError()));
	}
	std::vector<std::vector<ProcessorTarget>> cores;
	auto* cursor{storage.data()};
	const auto* end{storage.data() + bytes};
	while (cursor < end) {
		const auto* item{reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(cursor)};
		if (item->Size == 0 || cursor + item->Size > end)
			throw std::runtime_error("Windows returned malformed processor topology data.");
		if (item->Relationship == RelationProcessorCore) {
			std::vector<ProcessorTarget> core;
			for (WORD group_index{}; group_index < item->Processor.GroupCount; ++group_index) {
				const auto& group{item->Processor.GroupMask[group_index]};
				for (unsigned bit{}; bit < sizeof(KAFFINITY) * CHAR_BIT; ++bit) {
					if ((group.Mask & (KAFFINITY{1} << bit)) != 0)
						core.push_back({group.Group, static_cast<std::uint16_t>(bit)});
				}
			}
			std::sort(core.begin(), core.end(), [](const auto& left, const auto& right) {
				return left.group < right.group ||
					(left.group == right.group && left.number < right.number);
			});
			if (!core.empty()) cores.push_back(std::move(core));
		}
		cursor += item->Size;
	}
	if (cores.empty()) throw std::runtime_error("Windows reported no physical processor cores.");
	return cores;
}
#endif

AffinityPlan makeAffinityPlan(std::string policy, std::size_t cpu_count,
							  std::size_t gpu_count) {
	AffinityPlan plan;
	plan.policy = std::move(policy);
	plan.cpu_workers.resize(cpu_count);
	plan.gpu_workers.resize(gpu_count);
	if (plan.policy == "none") {
		plan.assignments.push_back("scheduler-managed");
		return plan;
	}
	if (plan.policy != "physical-first" && plan.policy != "bz-like" &&
		plan.policy != "reserve-core") {
		throw std::invalid_argument(
			"BRMINER_EXPERIMENT_AFFINITY must be none, physical-first, bz-like, or reserve-core.");
	}
#if !defined(_WIN32)
	throw std::runtime_error("Experimental thread-affinity policies currently require Windows.");
#else
	const auto cores{processorCoreTopology()};
	std::size_t logical_count{}, maximum_siblings{};
	for (const auto& core : cores) {
		logical_count += core.size();
		maximum_siblings = std::max(maximum_siblings, core.size());
	}
	std::vector<ProcessorTarget> physical_first;
	physical_first.reserve(logical_count);
	for (std::size_t sibling{}; sibling < maximum_siblings; ++sibling) {
		for (const auto& core : cores) {
			if (sibling < core.size()) physical_first.push_back(core[sibling]);
		}
	}
	plan.assignments.push_back("topology=" + std::to_string(cores.size()) +
		"c/" + std::to_string(logical_count) + "t");

	std::vector<ProcessorTarget> cpu_targets;
	std::vector<ProcessorTarget> gpu_targets;
	if (plan.policy == "physical-first") {
		if (cpu_count > physical_first.size())
			throw std::invalid_argument("physical-first affinity has fewer logical processors than CPU workers.");
		cpu_targets.assign(physical_first.begin(), physical_first.begin() + cpu_count);
	}
	else if (plan.policy == "bz-like") {
		if (gpu_count == 0)
			throw std::invalid_argument("bz-like affinity requires at least one GPU feeder.");
		if (gpu_count > physical_first.size() ||
			cpu_count > physical_first.size() - gpu_count) {
			throw std::invalid_argument(
				"bz-like affinity requires one unused logical processor per GPU feeder.");
		}
		cpu_targets.assign(physical_first.begin(), physical_first.begin() + cpu_count);
		gpu_targets.assign(physical_first.end() - gpu_count, physical_first.end());
	}
	else {
		if (gpu_count == 0)
			throw std::invalid_argument("reserve-core affinity requires at least one GPU feeder.");
		const auto& reserved{cores.back()};
		if (gpu_count > reserved.size())
			throw std::invalid_argument("The reserved physical core has too few logical processors for the GPU feeders.");
		for (const auto& target : physical_first) {
			const bool on_reserved_core{std::any_of(reserved.begin(), reserved.end(),
				[&](const auto& item) {
					return item.group == target.group && item.number == target.number;
				})};
			if (!on_reserved_core) cpu_targets.push_back(target);
		}
		if (cpu_count > cpu_targets.size())
			throw std::invalid_argument(
				"reserve-core affinity has fewer non-reserved logical processors than CPU workers.");
		cpu_targets.resize(cpu_count);
		gpu_targets.assign(reserved.begin(), reserved.begin() + gpu_count);
	}
	for (std::size_t i{}; i < cpu_targets.size(); ++i) {
		plan.cpu_workers[i] = cpu_targets[i];
		plan.assignments.push_back("cpu[" + std::to_string(i) + "]=" +
			processorTargetName(cpu_targets[i]));
	}
	for (std::size_t i{}; i < gpu_targets.size(); ++i) {
		plan.gpu_workers[i] = gpu_targets[i];
		plan.assignments.push_back("gpu[" + std::to_string(i) + "]=" +
			processorTargetName(gpu_targets[i]));
	}
	return plan;
#endif
}

void applyCurrentThreadAffinity(const std::optional<ProcessorTarget>& target) {
	if (!target) return;
#if defined(_WIN32)
	if (target->number >= sizeof(KAFFINITY) * CHAR_BIT)
		throw std::runtime_error("Processor number is outside the Windows group mask.");
	GROUP_AFFINITY affinity{};
	affinity.Group = target->group;
	affinity.Mask = KAFFINITY{1} << target->number;
	if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
		throw std::runtime_error("Could not apply thread affinity to " +
			processorTargetName(*target) + ": " + std::to_string(GetLastError()));
	}
#else
	throw std::runtime_error("Experimental thread affinity is unavailable on this platform.");
#endif
}

void validateAddress(std::string_view address) {
	if (address.empty() || address.size() > 160 ||
		!std::all_of(address.begin(), address.end(), [](unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				   (ch >= '0' && ch <= '9');
		})) {
		throw std::invalid_argument("PAYOUT_ADDRESS must be a Warthog wallet address containing only letters and digits.");
	}
}

std::vector<std::uint8_t> decodeHex(std::string_view text, std::size_t exact_bytes = 0) {
	if ((text.size() & 1U) != 0 || (exact_bytes != 0 && text.size() != exact_bytes * 2)) {
		throw std::runtime_error("Stratum server sent a malformed hexadecimal field.");
	}
	const auto nibble = [](char value) -> std::uint8_t {
		if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
		if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
		if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
		throw std::runtime_error("Stratum server sent a non-hexadecimal field.");
	};
	std::vector<std::uint8_t> result(text.size() / 2);
	for (std::size_t i{}; i < result.size(); ++i) {
		result[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
	}
	return result;
}

std::string encodeHex(const std::uint8_t* data, std::size_t size) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(size * 2, '0');
	for (std::size_t i{}; i < size; ++i) {
		result[i * 2] = digits[data[i] >> 4];
		result[i * 2 + 1] = digits[data[i] & 0x0f];
	}
	return result;
}

struct Notify {
	std::string job_id;
	std::array<std::uint8_t, 32> previous_hash{};
	std::vector<std::uint8_t> merkle_prefix;
	std::array<std::uint8_t, 4> version{};
	std::array<std::uint8_t, 4> target{};
	std::array<std::uint8_t, 4> timestamp{};
	bool clean{};
};

struct MiningJob {
	Header header{};
	std::string pool_job_id;
	std::vector<std::uint8_t> extra_nonce2;
	double share_difficulty{};
	std::uint64_t id{};
};

struct MinedShare {
	std::string pool_job_id;
	std::vector<std::uint8_t> extra_nonce2;
	std::array<std::uint8_t, 4> timestamp{};
	std::array<std::uint8_t, 4> nonce{};
	bool block_candidate{};
};

struct GpuCandidateBatch {
	std::shared_ptr<const MiningJob> job;
	std::vector<Sha256tCandidate> candidates;
	std::size_t next{};
};

struct RingCandidateBatch {
	std::shared_ptr<const MiningJob> job;
	std::array<Sha256tCandidate, kGpuCandidateTakeBatch> candidates{};
	std::size_t count{};
};

struct MinerState {
	std::mutex mutex;
	std::condition_variable changed;
	std::shared_ptr<const MiningJob> job;
	std::deque<GpuCandidateBatch> gpu_candidates;
	std::size_t gpu_candidate_count{};
	std::unique_ptr<BoundedMpmcRing<RingCandidateBatch>> gpu_candidate_ring;
	std::atomic<std::size_t> ring_candidate_count{};
	std::atomic<std::uint64_t> active_job_id{};
	std::size_t queue_limit{kGpuCandidateQueueLimit};
	std::atomic<bool> gpu_candidates_available{false};
	std::atomic<std::uint32_t> gpu_candidate_threshold{candidateThreshold(kInitialGpuCandidateFraction)};
	std::atomic<double> gpu_candidate_floor{kInitialGpuCandidateFraction};
	std::atomic<std::uint64_t> filter_adjustments{0};
	std::atomic<std::uint32_t> filter_minimum_threshold{UINT32_MAX};
	std::atomic<std::uint32_t> filter_maximum_threshold{0};
	std::uint64_t next_job_id{};
	std::atomic<std::uint64_t> next_nonce{0};
	std::atomic<std::uint64_t> cpu_sha256t_hashes{0};
	std::atomic<std::uint64_t> gpu_sha256t_hashes{0};
	std::atomic<std::uint64_t> verushash_hashes{0};
	std::atomic<std::uint64_t> gpu_candidates_found{0};
	std::atomic<std::uint64_t> gpu_scan_nanos{0};
	std::atomic<std::uint64_t> gpu_queue_wait_nanos{0};
	std::atomic<std::uint64_t> queue_consumer_lock_wait_nanos{0};
	std::atomic<std::uint64_t> queue_consumer_lock_hold_nanos{0};
	std::atomic<std::uint64_t> queue_consumer_lock_acquisitions{0};
	std::atomic<std::uint64_t> queue_empty_checks{0};
	std::atomic<std::uint64_t> queue_fast_empty_checks{0};
	std::atomic<std::uint64_t> queue_batches_taken{0};
	std::atomic<std::uint64_t> queue_producer_lock_wait_nanos{0};
	std::atomic<std::uint64_t> queue_producer_lock_hold_nanos{0};
	std::atomic<std::uint64_t> queue_producer_lock_acquisitions{0};
	std::atomic<std::uint64_t> queue_high_water_candidates{0};
	std::atomic<std::uint64_t> cuda_batches_submitted{0};
	std::atomic<std::uint64_t> cuda_batches_completed{0};
	std::atomic<std::uint64_t> cuda_kernel_launches{0};
	std::atomic<std::uint64_t> cuda_overflow_retries{0};
	std::atomic<std::uint64_t> cuda_buffer_growths{0};
	std::atomic<std::uint64_t> cuda_host_wait_nanos{0};
	std::atomic<std::uint32_t> gpu_workers_ready{0};
	std::string gpu_error;
	std::atomic<std::uint64_t> accepted_shares{0};
	std::atomic<std::uint64_t> rejected_shares{0};
	std::atomic<double> share_difficulty{0.0};
	std::atomic<double> suggested_share_difficulty{0.0};
	std::atomic<std::uint64_t> accepted_blocks{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> connected{false};
	std::atomic<bool> solution_ready{false};
	std::optional<MinedShare> solution;
	MiningConsole console;
	bool benchmark{};
	bool instrument_queue{};
	bool queue_fastpath{};
	bool queue_ring{};
	std::atomic<bool> worker_failed{false};
};

std::size_t candidateQueueCount(const MinerState& state) {
	return state.queue_ring
		? state.ring_candidate_count.load(std::memory_order_relaxed)
		: state.gpu_candidate_count;
}

bool candidateQueueNotEmpty(const MinerState& state) {
	return state.queue_ring
		? state.ring_candidate_count.load(std::memory_order_acquire) != 0
		: !state.gpu_candidates.empty();
}

void recordWorkerFailure(MinerState& state, std::string message) {
	state.worker_failed.store(true, std::memory_order_relaxed);
	{
		std::lock_guard lock{state.mutex};
		if (!state.gpu_error.empty()) state.gpu_error += "; ";
		state.gpu_error += std::move(message);
	}
	state.changed.notify_all();
}

void recordSolution(MinerState& state, const std::shared_ptr<const MiningJob>& job,
					std::uint32_t nonce, const PowResult& result) {
	if (state.benchmark) return;
	if (!result.meets_target && !result.meets_share_target) return;
	MinedShare share;
	share.pool_job_id = job->pool_job_id;
	share.extra_nonce2 = job->extra_nonce2;
	share.block_candidate = result.meets_target;
	std::copy_n(job->header.begin() + 72, share.timestamp.size(), share.timestamp.begin());
	share.nonce = {static_cast<std::uint8_t>(nonce), static_cast<std::uint8_t>(nonce >> 8),
				   static_cast<std::uint8_t>(nonce >> 16), static_cast<std::uint8_t>(nonce >> 24)};
	std::lock_guard lock{state.mutex};
	if (state.job && state.job->id == job->id && !state.solution) {
		state.solution = std::move(share);
		state.solution_ready.store(true, std::memory_order_relaxed);
		state.changed.notify_all();
		state.console.event(result.meets_target ? "BLOCK FOUND: queued for submission." : "Share found: queued for submission.");
	}
}

bool takeGpuCandidates(MinerState& state, std::shared_ptr<const MiningJob>& job,
					   std::vector<Sha256tCandidate>& batch) {
	if (state.queue_ring) {
		batch.clear();
		for (;;) {
			std::shared_ptr<const MiningJob> queued_job;
			std::size_t queued_count{};
			const bool dequeued{state.gpu_candidate_ring->tryDequeueWith(
				[&](RingCandidateBatch& queued) noexcept {
					queued_job = std::move(queued.job);
					queued_count = queued.count;
					batch.resize(queued.count);
					std::copy_n(queued.candidates.begin(), queued.count, batch.begin());
				})};
			if (!dequeued) return false;
			state.ring_candidate_count.fetch_sub(queued_count, std::memory_order_relaxed);
			if (!queued_job || queued_job->id !=
				state.active_job_id.load(std::memory_order_acquire)) continue;
			job = std::move(queued_job);
			return !batch.empty();
		}
	}
	if (state.queue_fastpath &&
		!state.gpu_candidates_available.load(std::memory_order_acquire)) {
		batch.clear();
		if (state.instrument_queue)
			state.queue_fast_empty_checks.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const auto lock_requested{state.instrument_queue ? std::chrono::steady_clock::now() :
		std::chrono::steady_clock::time_point{}};
	std::unique_lock lock{state.mutex};
	const auto lock_acquired{state.instrument_queue ? std::chrono::steady_clock::now() :
		std::chrono::steady_clock::time_point{}};
	const bool queue_was_limited{state.gpu_candidate_count > state.queue_limit};
	batch.clear();
	while (!state.gpu_candidates.empty() &&
		   (!state.job || state.gpu_candidates.front().job->id != state.job->id)) {
		const auto& stale{state.gpu_candidates.front()};
		state.gpu_candidate_count -= stale.candidates.size() - stale.next;
		state.gpu_candidates.pop_front();
	}
	if (!state.gpu_candidates.empty()) {
		auto& front{state.gpu_candidates.front()};
		job = front.job;
		const auto count{std::min(kGpuCandidateTakeBatch, front.candidates.size() - front.next)};
		batch.insert(batch.end(),
			std::make_move_iterator(front.candidates.begin() + front.next),
			std::make_move_iterator(front.candidates.begin() + front.next + count));
		front.next += count;
		state.gpu_candidate_count -= count;
		if (front.next == front.candidates.size()) state.gpu_candidates.pop_front();
	}
	if (queue_was_limited && state.gpu_candidate_count <= state.queue_limit) {
		state.changed.notify_all();
	}
	if (state.queue_fastpath && state.gpu_candidate_count == 0 &&
		state.gpu_candidates_available.load(std::memory_order_relaxed)) {
		state.gpu_candidates_available.store(false, std::memory_order_release);
	}
	const bool took_batch{!batch.empty()};
	if (state.instrument_queue) {
		const auto finished{std::chrono::steady_clock::now()};
		state.queue_consumer_lock_wait_nanos.fetch_add(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				lock_acquired - lock_requested).count(), std::memory_order_relaxed);
		state.queue_consumer_lock_hold_nanos.fetch_add(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				finished - lock_acquired).count(), std::memory_order_relaxed);
		state.queue_consumer_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
		(took_batch ? state.queue_batches_taken : state.queue_empty_checks)
			.fetch_add(1, std::memory_order_relaxed);
	}
	return took_batch;
}

bool enqueueRingCandidates(MinerState& state,
		const std::shared_ptr<const MiningJob>& job,
		const std::vector<Sha256tCandidate>& candidates) {
	std::size_t offset{};
	auto wait_started{std::chrono::steady_clock::time_point{}};
	const auto record_wait = [&] {
		if (wait_started != std::chrono::steady_clock::time_point{}) {
			state.gpu_queue_wait_nanos.fetch_add(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - wait_started).count(),
				std::memory_order_relaxed);
		}
	};
	while (offset < candidates.size()) {
		const std::size_t published_count{
			std::min(kGpuCandidateTakeBatch, candidates.size() - offset)};
		for (;;) {
			if (state.stop.load(std::memory_order_relaxed) ||
				state.solution_ready.load(std::memory_order_relaxed) ||
				state.active_job_id.load(std::memory_order_acquire) != job->id) {
				record_wait();
				return false;
			}
			bool became_nonempty{};
			const bool published{state.gpu_candidate_ring->tryEnqueueWith(
				[&](RingCandidateBatch& queued) noexcept {
					queued.job = job;
					queued.count = published_count;
					std::copy_n(candidates.begin() + offset, published_count,
						queued.candidates.begin());
				},
				[&state, published_count, &became_nonempty]() noexcept {
					const auto previous{state.ring_candidate_count.fetch_add(
						published_count, std::memory_order_relaxed)};
					became_nonempty = previous == 0;
					const auto depth{previous + published_count};
					auto high_water{state.queue_high_water_candidates.load(std::memory_order_relaxed)};
					while (high_water < depth &&
						!state.queue_high_water_candidates.compare_exchange_weak(
							high_water, depth, std::memory_order_relaxed)) {}
				})};
			if (published) {
				if (became_nonempty) state.changed.notify_all();
				break;
			}
			if (wait_started == std::chrono::steady_clock::time_point{})
				wait_started = std::chrono::steady_clock::now();
			std::this_thread::yield();
		}
		offset += published_count;
	}
	record_wait();
	return true;
}

Header gpuCandidateHeader(const MiningJob& job, const Sha256tCandidate& candidate) {
    Header candidate_header{job.header};
	candidate_header[76] = static_cast<std::uint8_t>(candidate.nonce);
	candidate_header[77] = static_cast<std::uint8_t>(candidate.nonce >> 8);
	candidate_header[78] = static_cast<std::uint8_t>(candidate.nonce >> 16);
	candidate_header[79] = static_cast<std::uint8_t>(candidate.nonce >> 24);
	return candidate_header;
}

bool confirmGpuCandidate(MinerState& state, const std::shared_ptr<const MiningJob>& job,
		const Sha256tCandidate& candidate, const Header& candidate_header,
		const PowResult& result) {
	if (!result.sha256t_candidate) return false;
	if (!result.meets_target && !result.meets_share_target) return true;
	const auto full_sha_hash{sha256t(candidate_header)};
	const std::uint32_t confirmed_word{(static_cast<std::uint32_t>(full_sha_hash[0]) << 24) |
		(static_cast<std::uint32_t>(full_sha_hash[1]) << 16) |
		(static_cast<std::uint32_t>(full_sha_hash[2]) << 8) | full_sha_hash[3]};
	if (confirmed_word != candidate.sha_word) {
		state.console.event("ERROR: GPU SHA256t candidate failed CPU confirmation.");
		return true;
	}
	const PowResult confirmed{checkJanusHash(candidate_header, full_sha_hash,
									 job->share_difficulty)};
	recordSolution(state, job, candidate.nonce, confirmed);
	return true;
}

std::vector<std::uint8_t> makeExtraNonce2(std::uint64_t counter, std::uint32_t size) {
	if (size < 1 || size > sizeof(counter)) {
		throw std::runtime_error("This pool's extranonce2 size is unsupported; expected 1 to 8 bytes.");
	}
	std::array<std::uint8_t, sizeof(counter)> encoded{};
	for (std::size_t i{}; i < encoded.size(); ++i) {
		encoded[encoded.size() - i - 1] = static_cast<std::uint8_t>(counter >> (i * 8));
	}
	return {encoded.end() - size, encoded.end()};
}

MiningJob buildJob(const Notify& notify, const std::vector<std::uint8_t>& extra_nonce1,
				   std::uint32_t extra_nonce2_size, std::uint64_t extra_nonce_counter,
				   double difficulty, std::uint64_t job_id) {
	MiningJob job;
	job.pool_job_id = notify.job_id;
	job.extra_nonce2 = makeExtraNonce2(extra_nonce_counter, extra_nonce2_size);
	job.share_difficulty = difficulty;
	job.id = job_id;
	std::copy(notify.previous_hash.begin(), notify.previous_hash.end(), job.header.begin());
	std::copy(notify.target.begin(), notify.target.end(), job.header.begin() + 32);
	std::vector<std::uint8_t> merkle_input;
	merkle_input.reserve(notify.merkle_prefix.size() + extra_nonce1.size() + job.extra_nonce2.size());
	merkle_input.insert(merkle_input.end(), notify.merkle_prefix.begin(), notify.merkle_prefix.end());
	merkle_input.insert(merkle_input.end(), extra_nonce1.begin(), extra_nonce1.end());
	merkle_input.insert(merkle_input.end(), job.extra_nonce2.begin(), job.extra_nonce2.end());
	std::array<std::uint8_t, 32> merkle_root{};
	CSHA256{}.Write(merkle_input.data(), merkle_input.size()).Finalize(merkle_root.data());
	std::copy(merkle_root.begin(), merkle_root.end(), job.header.begin() + 36);
	std::copy(notify.version.begin(), notify.version.end(), job.header.begin() + 68);
	std::copy(notify.timestamp.begin(), notify.timestamp.end(), job.header.begin() + 72);
	return job;
}

class WinsockRuntime {
public:
	WinsockRuntime() {
#if defined(_WIN32)
		WSADATA data{};
		const int result{WSAStartup(MAKEWORD(2, 2), &data)};
		if (result != 0) throw std::runtime_error("Could not initialize Windows networking: " + std::to_string(result));
#else
		std::signal(SIGPIPE, SIG_IGN);
#endif
	}
	~WinsockRuntime() {
#if defined(_WIN32)
		WSACleanup();
#endif
	}
	WinsockRuntime(const WinsockRuntime&) = delete;
	WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class SocketHandle {
public:
	explicit SocketHandle(SOCKET value = INVALID_SOCKET) : value_{value} {}
	~SocketHandle() { if (value_ != INVALID_SOCKET) closesocket(value_); }
	SocketHandle(const SocketHandle&) = delete;
	SocketHandle& operator=(const SocketHandle&) = delete;
	SocketHandle(SocketHandle&& other) noexcept : value_{other.value_} { other.value_ = INVALID_SOCKET; }
	SOCKET get() const { return value_; }
	explicit operator bool() const { return value_ != INVALID_SOCKET; }
private:
	SOCKET value_;
};

SocketHandle connectPool(const brminer::PoolEndpoint& endpoint) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfo* addresses{};
	const int lookup{getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &addresses)};
	if (lookup != 0) {
		throw std::runtime_error("Could not resolve pool " + endpoint.host + ":" + endpoint.port +
			" (Winsock " + std::to_string(lookup) + ").");
	}
	struct AddressList {
		addrinfo* value;
		~AddressList() { freeaddrinfo(value); }
	} address_list{addresses};

	int last_error{};
	for (addrinfo* address{addresses}; address; address = address->ai_next) {
		SocketHandle socket{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
		if (!socket) {
			last_error = WSAGetLastError();
			continue;
		}
#if defined(_WIN32)
		DWORD timeout{250};
#else
		timeval timeout{0, 250000};
#endif
		setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
				   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		if (::connect(socket.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
			return socket;
		}
		last_error = WSAGetLastError();
	}
	throw std::runtime_error("Could not connect to pool " + endpoint.host + ":" + endpoint.port +
		" (Winsock " + std::to_string(last_error) + ").");
}

void sendAll(SOCKET socket, std::string_view data) {
	std::size_t sent{};
	while (sent < data.size()) {
		const auto count{send(socket, data.data() + sent,
							 static_cast<int>(std::min<std::size_t>(data.size() - sent, INT_MAX)), 0)};
		if (count == SOCKET_ERROR || count == 0) {
			throw std::runtime_error("Stratum send failed (Winsock " + std::to_string(WSAGetLastError()) + ").");
		}
		sent += static_cast<std::size_t>(count);
	}
}

enum class ReadResult { line, timeout, closed };

ReadResult readLine(SOCKET socket, std::string& buffer, std::string& line) {
	for (;;) {
		const auto newline{buffer.find('\n')};
		if (newline != std::string::npos) {
			line.assign(buffer, 0, newline);
			buffer.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			return ReadResult::line;
		}
		if (buffer.size() > 1024 * 1024) throw std::runtime_error("Stratum message exceeded the 1 MiB safety limit.");
		std::array<char, 4096> bytes{};
		const auto received{recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0)};
		if (received == 0) return ReadResult::closed;
		if (received == SOCKET_ERROR) {
			const int error{WSAGetLastError()};
			if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) return ReadResult::timeout;
			throw std::runtime_error("Stratum receive failed (Winsock " + std::to_string(error) + ").");
		}
		buffer.append(bytes.data(), static_cast<std::size_t>(received));
	}
}

std::string stratumRequest(std::uint32_t id, std::string_view method,
						   const std::vector<std::string>& params) {
		UniValue message{UniValue::VOBJ};
		UniValue json_params{UniValue::VARR};
		for (const auto& param : params) json_params.push_back(UniValue{param});
		message.pushKV("id", static_cast<std::int64_t>(id));
		message.pushKV("method", std::string{method});
		message.pushKV("params", std::move(json_params));
		return message.write() + "\n";
}

std::string stratumSubmit(const MinedShare& share, std::uint32_t id) {
	return stratumRequest(id, "mining.submit", {
		share.pool_job_id,
		encodeHex(share.extra_nonce2.data(), share.extra_nonce2.size()),
		encodeHex(share.timestamp.data(), share.timestamp.size()),
		encodeHex(share.nonce.data(), share.nonce.size())});
}

std::string stratumDifficultySuggestion(std::uint32_t id, double difficulty) {
	std::ostringstream message;
	message << "{\"id\":" << id << ",\"method\":\"mining.suggest_difficulty\",\"params\":["
			<< std::fixed << std::setprecision(0) << difficulty << "]}\n";
	return message.str();
}

std::int64_t requireInteger(const UniValue& object, const char* key) {
	const UniValue& value{object[key]};
	if (!value.isNum()) throw std::runtime_error(std::string{"Stratum response missing numeric field: "} + key);
	return value.getInt<std::int64_t>();
}

Notify parseNotify(const UniValue& params) {
	if (!params.isArray() || params.size() != 7 || !params[0].isStr() || !params[1].isStr() ||
		!params[2].isStr() || !params[3].isStr() || !params[4].isStr() || !params[5].isStr() ||
		!params[6].isBool()) {
		throw std::runtime_error("Pool sent an invalid mining.notify message.");
	}
	Notify notify;
	notify.job_id = params[0].get_str();
	const auto previous_hash{decodeHex(params[1].get_str(), notify.previous_hash.size())};
	const auto merkle{decodeHex(params[2].get_str())};
	const auto version{decodeHex(params[3].get_str(), notify.version.size())};
	const auto target{decodeHex(params[4].get_str(), notify.target.size())};
	const auto timestamp{decodeHex(params[5].get_str(), notify.timestamp.size())};
	std::copy(previous_hash.begin(), previous_hash.end(), notify.previous_hash.begin());
	notify.merkle_prefix = merkle;
	std::copy(version.begin(), version.end(), notify.version.begin());
	std::copy(target.begin(), target.end(), notify.target.begin());
	std::copy(timestamp.begin(), timestamp.end(), notify.timestamp.begin());
	notify.clean = params[6].get_bool();
	return notify;
}

void publishNotify(MinerState& state, const Notify& notify, const std::vector<std::uint8_t>& extra_nonce1,
				   std::uint32_t extra_nonce2_size, std::uint64_t& extra_nonce_counter,
				   double difficulty) {
	std::lock_guard lock{state.mutex};
	++extra_nonce_counter;
	MiningJob job{buildJob(notify, extra_nonce1, extra_nonce2_size,
						  extra_nonce_counter, difficulty, ++state.next_job_id)};
	state.next_nonce.store(0, std::memory_order_relaxed);
	if (!state.queue_ring) {
		state.gpu_candidates.clear();
		state.gpu_candidate_count = 0;
		if (state.queue_fastpath)
			state.gpu_candidates_available.store(false, std::memory_order_release);
	}
	state.solution.reset();
	state.solution_ready.store(false, std::memory_order_relaxed);
	state.job = std::make_shared<const MiningJob>(std::move(job));
	state.active_job_id.store(state.job->id, std::memory_order_release);
	state.changed.notify_all();
}

void handleStratumMessage(MinerState& state, const UniValue& message, bool& subscribed, bool& authorized,
						  std::vector<std::uint8_t>& extra_nonce1, std::uint32_t& extra_nonce2_size,
						  std::uint64_t& extra_nonce_counter, double& difficulty,
						  std::optional<Notify>& latest_notify,
						  std::optional<std::uint32_t>& submit_id, bool& pending_block_candidate,
						  std::optional<std::uint32_t>& difficulty_suggestion_id,
						  bool& difficulty_suggestion_supported) {
	if (!message.isObject()) throw std::runtime_error("Pool sent a non-object Stratum message.");
	const UniValue& id_value{message["id"]};
	if (!id_value.isNull()) {
		const auto id{requireInteger(message, "id")};
		const UniValue& error{message["error"]};
		const UniValue& result{message["result"]};
		if (submit_id && id == *submit_id) {
			if (error.isNull() && result.isBool() && result.get_bool()) {
				state.accepted_shares.fetch_add(1, std::memory_order_relaxed);
				if (pending_block_candidate) {
					state.accepted_blocks.fetch_add(1, std::memory_order_relaxed);
				}
				state.console.event(pending_block_candidate ? "BLOCK ACCEPTED by the Stratum server." : "Share accepted by the Stratum server.");
			}
			else {
				state.rejected_shares.fetch_add(1, std::memory_order_relaxed);
				state.console.event(std::string{pending_block_candidate ? "BLOCK REJECTED" : "Share rejected"} +
					(error.isNull() ? "." : ": " + error.write()));
			}
			state.console.shares(state.accepted_shares.load(std::memory_order_relaxed),
				state.rejected_shares.load(std::memory_order_relaxed),
				state.accepted_blocks.load(std::memory_order_relaxed));
			submit_id.reset();
			pending_block_candidate = false;
			state.connected.store(true, std::memory_order_relaxed);
			return;
		}
		if (difficulty_suggestion_id && id == *difficulty_suggestion_id) {
			if (!error.isNull() || !result.isBool() || !result.get_bool()) {
				difficulty_suggestion_supported = false;
			}
			difficulty_suggestion_id.reset();
			return;
		}
		if (!error.isNull()) {
			throw std::runtime_error("Pool Stratum request failed: " + error.write());
		}
		if (id == kSubscribeId) {
			if (!result.isArray() || result.size() < 3 || !result[1].isStr() || !result[2].isNum()) {
				throw std::runtime_error("Pool sent an invalid mining.subscribe response.");
			}
			extra_nonce1 = decodeHex(result[1].get_str());
			const auto size{result[2].getInt<std::uint32_t>()};
			if (size < 1 || size > 8) throw std::runtime_error("Pool extranonce2 size must be from 1 to 8 bytes.");
			extra_nonce2_size = size;
			subscribed = true;
			if (authorized && latest_notify) {
				publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
							  extra_nonce_counter, difficulty);
			}
		}
		else if (id == kAuthorizeId) {
			if (!result.isBool() || !result.get_bool()) {
				throw std::runtime_error("Pool rejected the Warthog wallet address.");
			}
			authorized = true;
			state.console.event("Payout address authorized; waiting for work.");
			if (subscribed && latest_notify) {
				publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
							  extra_nonce_counter, difficulty);
			}
		}
		return;
	}

	const UniValue& method_value{message["method"]};
	const UniValue& params{message["params"]};
	if (!method_value.isStr() || !params.isArray()) throw std::runtime_error("Pool sent a malformed Stratum notification.");
	const std::string method{method_value.get_str()};
	if (method == "mining.set_difficulty") {
		if (params.size() != 1 || !params[0].isNum()) throw std::runtime_error("Pool sent an invalid share difficulty.");
		difficulty = params[0].get_real();
		if (!std::isfinite(difficulty) || difficulty <= 0.0) throw std::runtime_error("Pool sent a non-positive share difficulty.");
		state.share_difficulty.store(difficulty, std::memory_order_relaxed);
		std::ostringstream difficulty_message;
		difficulty_message << "Share difficulty set to " << std::setprecision(5) << difficulty << '.';
		state.console.event(difficulty_message.str());
		if (latest_notify && subscribed && authorized) {
			publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
						  extra_nonce_counter, difficulty);
		}
	}
	else if (method == "mining.notify") {
		latest_notify = parseNotify(params);
		state.console.event("New job received: " + latest_notify->job_id);
		if (subscribed && authorized) {
			publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
						  extra_nonce_counter, difficulty);
		}
	}
	else if (method == "mining.set_extranonce") {
		if (params.size() != 2 || !params[0].isStr() || !params[1].isNum()) throw std::runtime_error("Pool sent an invalid extranonce update.");
		extra_nonce1 = decodeHex(params[0].get_str());
		extra_nonce2_size = params[1].getInt<std::uint32_t>();
		if (extra_nonce2_size < 1 || extra_nonce2_size > 8) throw std::runtime_error("Pool extranonce2 size must be from 1 to 8 bytes.");
		if (latest_notify && subscribed && authorized) {
			publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
						  extra_nonce_counter, difficulty);
		}
	}
}

void miningWorker(MinerState& state, bool verify_gpu_candidates) {
	std::uint64_t seen_job{};
	std::shared_ptr<const MiningJob> gpu_job;
	std::vector<Sha256tCandidate> gpu_batch;
	#if defined(BRMINER_NATIVE_BATCH)
	std::vector<PowResult> gpu_results;
	#endif
	if (verify_gpu_candidates) gpu_batch.reserve(kGpuCandidateTakeBatch);
	#if defined(BRMINER_NATIVE_BATCH)
	if (verify_gpu_candidates) gpu_results.reserve(kGpuCandidateTakeBatch);
	#endif
	while (!state.stop.load(std::memory_order_relaxed)) {
		std::shared_ptr<const MiningJob> job;
		{
			std::unique_lock lock{state.mutex};
			state.changed.wait(lock, [&] {
				return state.stop.load(std::memory_order_relaxed) ||
					   (state.job && !state.solution_ready.load(std::memory_order_relaxed) &&
						(state.next_nonce.load(std::memory_order_relaxed) <= UINT32_MAX ||
						 candidateQueueNotEmpty(state)));
			});
			if (state.stop.load(std::memory_order_relaxed)) return;
			job = state.job;
			seen_job = job->id;
		}

		std::uint64_t iterations{};
		while (!state.stop.load(std::memory_order_relaxed) &&
			   !state.solution_ready.load(std::memory_order_relaxed)) {
			if ((iterations++ & 0x3ffU) == 0) {
				std::lock_guard lock{state.mutex};
				if (!state.job || state.job->id != seen_job) break;
			}
			if (verify_gpu_candidates) {
				if (takeGpuCandidates(state, gpu_job, gpu_batch)) {
					std::uint64_t verified{};
					#if defined(BRMINER_NATIVE_BATCH)
					checkJanusHashFromShaWordBatch(gpu_job->header, gpu_batch,
						gpu_job->share_difficulty, gpu_results);
					#endif
					for (std::size_t candidate_index{}; candidate_index < gpu_batch.size(); ++candidate_index) {
						if ((candidate_index & 63U) == 0 &&
							(state.stop.load(std::memory_order_relaxed) ||
							 state.solution_ready.load(std::memory_order_relaxed))) break;
						#if defined(BRMINER_NATIVE_BATCH)
						const auto result{gpu_results[candidate_index]};
						if (!result.meets_target && !result.meets_share_target) {
							verified += result.sha256t_candidate;
							continue;
						}
						const auto header{gpuCandidateHeader(*gpu_job, gpu_batch[candidate_index])};
						#else
						const auto header{gpuCandidateHeader(*gpu_job, gpu_batch[candidate_index])};
						const auto result{checkJanusHashFromShaWord(header,
							gpu_batch[candidate_index].sha_word, gpu_job->share_difficulty)};
						#endif
						verified += confirmGpuCandidate(state, gpu_job, gpu_batch[candidate_index],
							header, result);
					}
					if (verified != 0) {
						state.verushash_hashes.fetch_add(verified, std::memory_order_relaxed);
					}
					continue;
				}
			}
			const std::uint64_t nonce{state.next_nonce.fetch_add(1, std::memory_order_relaxed)};
			if (nonce > UINT32_MAX) break;

			Header candidate{job->header};
			const auto nonce_value{static_cast<std::uint32_t>(nonce)};
			candidate[76] = static_cast<std::uint8_t>(nonce_value);
			candidate[77] = static_cast<std::uint8_t>(nonce_value >> 8);
			candidate[78] = static_cast<std::uint8_t>(nonce_value >> 16);
			candidate[79] = static_cast<std::uint8_t>(nonce_value >> 24);

			state.cpu_sha256t_hashes.fetch_add(1, std::memory_order_relaxed);
			const PowResult result{checkJanusHash(candidate, job->share_difficulty)};
			if (!result.sha256t_candidate) continue;
			state.verushash_hashes.fetch_add(1, std::memory_order_relaxed);
			if (!result.meets_target && !result.meets_share_target) continue;
			recordSolution(state, job, nonce_value, result);
			break;
		}
	}
}

void gpuWorker(MinerState& state, std::size_t device_index,
               bool use_cuda, std::string nvrtc_dll, std::uint32_t cuda_batch_size,
               std::uint32_t cuda_block_size, std::string cuda_pipeline,
			   FilterPolicy filter_policy) {
	bool registered{};
	try {
                std::unique_ptr<OpenClSha256tMiner> opencl_miner;
                std::unique_ptr<CudaSha256tMiner> cuda_miner;
                if (use_cuda) {
                        cuda_miner = std::make_unique<CudaSha256tMiner>(
                                device_index, nvrtc_dll, cuda_block_size);
                }
                else opencl_miner = std::make_unique<OpenClSha256tMiner>(device_index);
		std::unordered_map<std::uint64_t, std::shared_ptr<const MiningJob>> pending_jobs;
		std::uint64_t next_pipeline_tag{};
		auto last_threshold_adjustment{std::chrono::steady_clock::now()};
		auto last_cpu_sha_hashes{state.cpu_sha256t_hashes.load(std::memory_order_relaxed)};
		auto last_verus_hashes{state.verushash_hashes.load(std::memory_order_relaxed)};
		auto last_gpu_sha_hashes{state.gpu_sha256t_hashes.load(std::memory_order_relaxed)};
		auto last_gpu_scan_nanos{state.gpu_scan_nanos.load(std::memory_order_relaxed)};
		state.gpu_workers_ready.fetch_add(1, std::memory_order_relaxed);
		registered = true;
		while (!state.stop.load(std::memory_order_relaxed)) {
			std::shared_ptr<const MiningJob> job;
			{
				std::unique_lock lock{state.mutex};
				const bool queue_limited{state.job &&
					candidateQueueCount(state) > state.queue_limit};
				const auto wait_start{std::chrono::steady_clock::now()};
				state.changed.wait(lock, [&] {
					return state.stop.load(std::memory_order_relaxed) ||
						   (state.job && !state.solution_ready.load(std::memory_order_relaxed) &&
							state.next_nonce.load(std::memory_order_relaxed) <= UINT32_MAX &&
							candidateQueueCount(state) <= state.queue_limit);
				});
				if (queue_limited) {
					state.gpu_queue_wait_nanos.fetch_add(
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - wait_start).count(),
						std::memory_order_relaxed);
				}
				if (state.stop.load(std::memory_order_relaxed)) break;
				job = state.job;
			}
			const std::uint64_t batch_size{use_cuda ? cuda_batch_size : 4194304U};
			const std::uint64_t start{state.next_nonce.fetch_add(batch_size, std::memory_order_relaxed)};
			const auto scan_start{std::chrono::steady_clock::now()};
			std::uint32_t count{};
			std::vector<Sha256tCandidate> candidates;
			if (!use_cuda || cuda_pipeline == "sync") {
				if (start > UINT32_MAX) continue;
				count = static_cast<std::uint32_t>(std::min<std::uint64_t>(batch_size,
					static_cast<std::uint64_t>(UINT32_MAX) - start + 1));
				const auto threshold{state.gpu_candidate_threshold.load(std::memory_order_relaxed)};
				candidates = use_cuda
					? cuda_miner->scan(job->header, static_cast<std::uint32_t>(start), count, threshold)
					: opencl_miner->scan(job->header, static_cast<std::uint32_t>(start), count, threshold);
			} else {
				std::optional<CudaPipelineResult> completed;
				if (start <= UINT32_MAX) {
					count = static_cast<std::uint32_t>(std::min<std::uint64_t>(batch_size,
						static_cast<std::uint64_t>(UINT32_MAX) - start + 1));
					const auto threshold{state.gpu_candidate_threshold.load(std::memory_order_relaxed)};
					const auto tag{++next_pipeline_tag};
					pending_jobs.emplace(tag, job);
					completed = cuda_miner->submitPipelined(job->header,
						static_cast<std::uint32_t>(start), count, threshold, tag);
					if (cuda_pipeline == "depth1" && !completed)
						completed = cuda_miner->harvestPipelined();
				} else {
					completed = cuda_miner->harvestPipelined();
				}
				if (!completed) continue;
				const auto pending{pending_jobs.find(completed->tag)};
				if (pending == pending_jobs.end())
					throw std::logic_error("CUDA pipeline returned an unknown batch tag.");
				job = std::move(pending->second);
				pending_jobs.erase(pending);
				count = completed->nonce_count;
				candidates = std::move(completed->candidates);
			}
			if (state.benchmark && !candidates.empty()) {
				for (const auto index : {std::size_t{0}, candidates.size() / 2, candidates.size() - 1}) {
					Header check{job->header};
					const auto& candidate{candidates[index]};
					for (unsigned byte{}; byte < 4; ++byte)
						check[76 + byte] = static_cast<std::uint8_t>(candidate.nonce >> (byte * 8));
					const auto hash{sha256t(check)};
					const std::uint32_t word{(static_cast<std::uint32_t>(hash[0]) << 24) |
						(static_cast<std::uint32_t>(hash[1]) << 16) | (static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
					if (word != candidate.sha_word) throw std::runtime_error("Benchmark GPU sample failed CPU confirmation.");
				}
			}
			const std::size_t candidate_count{candidates.size()};
			state.gpu_scan_nanos.fetch_add(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - scan_start).count(),
				std::memory_order_relaxed);
			state.gpu_candidates_found.fetch_add(candidate_count, std::memory_order_relaxed);
			state.gpu_sha256t_hashes.fetch_add(count, std::memory_order_relaxed);
			bool enqueue_ring{};
			const auto queue_lock_requested{state.instrument_queue ? std::chrono::steady_clock::now() :
				std::chrono::steady_clock::time_point{}};
			{
				std::unique_lock lock{state.mutex};
				const auto queue_lock_acquired{state.instrument_queue ? std::chrono::steady_clock::now() :
					std::chrono::steady_clock::time_point{}};
				if (!state.stop.load(std::memory_order_relaxed) && state.job &&
					state.job->id == job->id && !state.solution_ready.load(std::memory_order_relaxed)) {
					const std::size_t queued_before{candidateQueueCount(state)};
					if (candidate_count != 0) {
						if (state.queue_ring) enqueue_ring = true;
						else {
							state.gpu_candidates.push_back({job, std::move(candidates)});
							state.gpu_candidate_count += candidate_count;
						}
					}
					const auto now{std::chrono::steady_clock::now()};
					if (device_index == 0 &&
						now - last_threshold_adjustment >= filter_policy.adjustment_interval) {
						const auto cpu_sha_hashes{state.cpu_sha256t_hashes.load(std::memory_order_relaxed)};
						const auto verus_hashes{state.verushash_hashes.load(std::memory_order_relaxed)};
						const auto gpu_sha_hashes{state.gpu_sha256t_hashes.load(std::memory_order_relaxed)};
						const auto gpu_scan_nanos{state.gpu_scan_nanos.load(std::memory_order_relaxed)};
						const auto cpu_sha_delta{cpu_sha_hashes - last_cpu_sha_hashes};
						const auto verus_delta{verus_hashes - last_verus_hashes};
						const auto gpu_sha_delta{gpu_sha_hashes - last_gpu_sha_hashes};
						const auto gpu_scan_delta{gpu_scan_nanos - last_gpu_scan_nanos};
						const double fraction{static_cast<double>(
							state.gpu_candidate_threshold.load(std::memory_order_relaxed) -
							kSha256tCandidateFloor) /
							static_cast<double>(UINT32_MAX - kSha256tCandidateFloor)};
						if (gpu_sha_delta != 0 && gpu_scan_delta != 0) {
							// GPU active-scan rate stays meaningful when queue backpressure
							// suspends launches; wall-clock production does not.
							const double interval_seconds{std::chrono::duration<double>(
								now - last_threshold_adjustment).count()};
							const double active_gpu_rate{static_cast<double>(gpu_sha_delta) *
								1e9 / static_cast<double>(gpu_scan_delta)};
							const double cpu_rate{static_cast<double>(verus_delta) / interval_seconds};
							const double queue_target{static_cast<double>(state.queue_limit) *
								filter_policy.queue_target_fraction};
							const double queue_correction{(queue_target -
								static_cast<double>(queued_before)) /
								filter_policy.drain_horizon_seconds};
							double desired_rate{std::max(1.0, cpu_rate + queue_correction)};
							if (cpu_sha_delta > std::max<std::uint64_t>(10000, verus_delta / 100) &&
								static_cast<double>(queued_before) < queue_target) {
								desired_rate = std::max(desired_rate, fraction * active_gpu_rate * 1.08);
							}
							const double target_fraction{std::clamp(desired_rate / active_gpu_rate,
								kInitialGpuCandidateFraction, 0.25)};
							const double adjusted_fraction{std::clamp(
								0.5 * fraction + 0.5 * target_fraction,
								kInitialGpuCandidateFraction, 0.25)};
							state.gpu_candidate_floor.store(adjusted_fraction, std::memory_order_relaxed);
							const auto threshold{candidateThreshold(adjusted_fraction)};
							state.gpu_candidate_threshold.store(threshold, std::memory_order_relaxed);
							state.filter_adjustments.fetch_add(1, std::memory_order_relaxed);
							auto minimum{state.filter_minimum_threshold.load(std::memory_order_relaxed)};
							while (threshold < minimum &&
								!state.filter_minimum_threshold.compare_exchange_weak(
									minimum, threshold, std::memory_order_relaxed)) {}
							auto maximum{state.filter_maximum_threshold.load(std::memory_order_relaxed)};
							while (threshold > maximum &&
								!state.filter_maximum_threshold.compare_exchange_weak(
									maximum, threshold, std::memory_order_relaxed)) {}
						}
						last_cpu_sha_hashes = cpu_sha_hashes;
						last_verus_hashes = verus_hashes;
						last_gpu_sha_hashes = gpu_sha_hashes;
						last_gpu_scan_nanos = gpu_scan_nanos;
						last_threshold_adjustment = now;
					}
				}
				if (state.queue_fastpath && state.gpu_candidate_count != 0 &&
					!state.gpu_candidates_available.load(std::memory_order_relaxed)) {
					state.gpu_candidates_available.store(true, std::memory_order_release);
				}
				if (state.instrument_queue) {
					const auto finished{std::chrono::steady_clock::now()};
					state.queue_producer_lock_wait_nanos.fetch_add(
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							queue_lock_acquired - queue_lock_requested).count(),
						std::memory_order_relaxed);
					state.queue_producer_lock_hold_nanos.fetch_add(
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							finished - queue_lock_acquired).count(),
						std::memory_order_relaxed);
					state.queue_producer_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
					auto high_water{state.queue_high_water_candidates.load(std::memory_order_relaxed)};
					while (high_water < state.gpu_candidate_count &&
						!state.queue_high_water_candidates.compare_exchange_weak(
							high_water, state.gpu_candidate_count, std::memory_order_relaxed)) {}
				}
			}
			if (enqueue_ring) enqueueRingCandidates(state, job, candidates);
			state.changed.notify_all();
		}
		if (cuda_miner) {
			const auto stats{cuda_miner->pipelineStats()};
			state.cuda_batches_submitted.fetch_add(stats.batches_submitted, std::memory_order_relaxed);
			state.cuda_batches_completed.fetch_add(stats.batches_completed, std::memory_order_relaxed);
			state.cuda_kernel_launches.fetch_add(stats.kernel_launches, std::memory_order_relaxed);
			state.cuda_overflow_retries.fetch_add(stats.overflow_retries, std::memory_order_relaxed);
			state.cuda_buffer_growths.fetch_add(stats.buffer_growths, std::memory_order_relaxed);
			state.cuda_host_wait_nanos.fetch_add(stats.host_wait_nanos, std::memory_order_relaxed);
		}
	}
	catch (const std::exception& error) {
		state.worker_failed.store(true, std::memory_order_relaxed);
		{
			std::lock_guard lock{state.mutex};
			if (!state.gpu_error.empty()) state.gpu_error += "; ";
			state.gpu_error += error.what();
		}
		state.changed.notify_all();
		state.console.event(std::string{"ERROR: GPU worker stopped: "} + error.what());
	}
	if (registered) state.gpu_workers_ready.fetch_sub(1, std::memory_order_relaxed);
}

void networkWorker(MinerState& state, std::string payout_address, brminer::PoolEndpoint endpoint) {
	const std::string endpoint_name{endpoint.host + ":" + endpoint.port};
	std::string last_error;
	auto last_error_time = std::chrono::steady_clock::time_point{};
	while (!state.stop.load(std::memory_order_relaxed)) {
		try {
			SocketHandle socket{connectPool(endpoint)};
			state.connected.store(true, std::memory_order_relaxed);
			state.console.event("Connected to " + endpoint_name + '.');
			sendAll(socket.get(), stratumRequest(kSubscribeId, "mining.subscribe", {"BRminer/1.0"}));
			sendAll(socket.get(), stratumRequest(kAuthorizeId, "mining.authorize", {payout_address, "x"}));

			bool subscribed{};
			bool authorized{};
			std::vector<std::uint8_t> extra_nonce1;
			std::uint32_t extra_nonce2_size{};
			std::uint64_t extra_nonce_counter{};
			double difficulty{1.0};
			std::optional<Notify> latest_notify;
			std::optional<std::uint32_t> submit_id;
			bool pending_block_candidate{};
			std::uint32_t next_id{3};
			std::optional<std::uint32_t> difficulty_suggestion_id;
			bool difficulty_suggestion_supported{true};
			double last_suggested_difficulty{};
			auto last_suggestion_time{std::chrono::steady_clock::time_point{}};
			std::string receive_buffer;
			while (!state.stop.load(std::memory_order_relaxed)) {
				const double suggested_difficulty{
					state.suggested_share_difficulty.load(std::memory_order_relaxed)};
				const auto now{std::chrono::steady_clock::now()};
				const bool changed_enough{last_suggested_difficulty == 0.0 ||
					std::abs(suggested_difficulty - last_suggested_difficulty) >=
						last_suggested_difficulty * 0.15};
				if (subscribed && authorized && latest_notify && difficulty_suggestion_supported &&
					!difficulty_suggestion_id && suggested_difficulty > 0.0 && changed_enough &&
					(now - last_suggestion_time >= std::chrono::seconds{30})) {
					difficulty_suggestion_id = next_id++;
					sendAll(socket.get(), stratumDifficultySuggestion(*difficulty_suggestion_id, suggested_difficulty));
					last_suggested_difficulty = suggested_difficulty;
					last_suggestion_time = now;
				}
				if (!submit_id) {
					std::optional<MinedShare> share;
					{
						std::lock_guard lock{state.mutex};
						if (state.solution) {
							share = std::move(state.solution);
							state.solution.reset();
							state.solution_ready.store(false, std::memory_order_relaxed);
							state.changed.notify_all();
						}
					}
					if (share) {
						submit_id = next_id++;
						pending_block_candidate = share->block_candidate;
						sendAll(socket.get(), stratumSubmit(*share, *submit_id));
						state.console.event(share->block_candidate ? "Block submitted; waiting for acceptance." : "Share submitted; waiting for acceptance.");
						state.connected.store(true, std::memory_order_relaxed);
					}
				}
				if (!submit_id && latest_notify && subscribed && authorized &&
					state.next_nonce.load(std::memory_order_relaxed) > UINT32_MAX) {
					bool solution_pending{};
					{
						std::lock_guard lock{state.mutex};
						solution_pending = state.solution.has_value() ||
							state.solution_ready.load(std::memory_order_relaxed);
					}
					if (!solution_pending) {
						publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
							extra_nonce_counter, difficulty);
					}
				}
				std::string line;
				const ReadResult read_result{readLine(socket.get(), receive_buffer, line)};
				if (read_result == ReadResult::closed) throw std::runtime_error("Pool closed the Stratum connection.");
				if (read_result == ReadResult::timeout) continue;
				UniValue message;
				if (!message.read(line)) throw std::runtime_error("Pool sent malformed Stratum JSON.");
				handleStratumMessage(state, message, subscribed, authorized, extra_nonce1,
								extra_nonce2_size, extra_nonce_counter, difficulty, latest_notify,
								submit_id, pending_block_candidate, difficulty_suggestion_id, difficulty_suggestion_supported);
			}
			return;
		}
		catch (const std::exception& error) {
			state.connected.store(false, std::memory_order_relaxed);
			{
				std::lock_guard lock{state.mutex};
				state.job.reset();
				state.active_job_id.store(0, std::memory_order_release);
				if (!state.queue_ring) {
					state.gpu_candidates.clear();
					state.gpu_candidate_count = 0;
					if (state.queue_fastpath)
						state.gpu_candidates_available.store(false, std::memory_order_release);
				}
				state.solution.reset();
				state.solution_ready.store(false, std::memory_order_relaxed);
				state.changed.notify_all();
			}
			const auto now{std::chrono::steady_clock::now()};
			if (last_error != error.what() || now - last_error_time >= std::chrono::seconds{10}) {
				state.console.event("ERROR: pool connection error (" + endpoint_name + "): " + error.what());
				last_error = error.what();
				last_error_time = now;
			}
			std::this_thread::sleep_for(std::chrono::seconds{2});
		}
	}
}

}  // namespace

namespace {

Header benchmarkHeader(std::uint64_t epoch) {
	Header header{};
	for (std::size_t i{}; i < header.size(); ++i) header[i] = static_cast<std::uint8_t>(i * 17 + 11);
	std::fill(header.begin() + 32, header.begin() + 44, 0);
	std::fill(header.begin() + 68, header.end(), 0);
	for (unsigned i{}; i < 8; ++i) header[36 + i] = static_cast<std::uint8_t>(epoch >> (i * 8));
	header[71] = 3;
	header[75] = 1;
	return header;
}

void publishBenchmarkJob(MinerState& state, std::uint64_t epoch) {
	auto job{std::make_shared<MiningJob>()};
	job->header = benchmarkHeader(epoch);
	job->share_difficulty = 1e12;
	std::lock_guard lock{state.mutex};
	job->id = ++state.next_job_id;
	state.job = std::move(job);
	state.active_job_id.store(state.job->id, std::memory_order_release);
	state.next_nonce.store(0, std::memory_order_relaxed);
	if (!state.queue_ring) {
		state.gpu_candidates.clear();
		state.gpu_candidate_count = 0;
		if (state.queue_fastpath)
			state.gpu_candidates_available.store(false, std::memory_order_release);
	}
	state.changed.notify_all();
}

void benchmarkWorker(MinerState& state) {
	std::uint64_t epoch{};
	publishBenchmarkJob(state, epoch);
	while (!state.stop.load(std::memory_order_relaxed)) {
		if (state.next_nonce.load(std::memory_order_relaxed) > UINT32_MAX) publishBenchmarkJob(state, ++epoch);
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	}
}

void verifyBenchmarkCpu() {
	const Header fixed{benchmarkHeader(0)};
	constexpr std::array<std::uint8_t, 32> known{0x0f,0x0f,0xe5,0x90,0x6b,0x83,0x55,0x2d,
		0x98,0x1d,0x2a,0x81,0x0e,0xbd,0x34,0x5b,0x13,0x78,0x6b,0x34,0x04,0x6b,0xb5,0xe6,
		0xe6,0xcf,0x2c,0x58,0xc3,0x6d,0x1d,0x69};
	if (sha256t(fixed) != known) throw std::runtime_error("Benchmark SHA256t known-answer check failed.");
}

template<typename Miner> void verifyBenchmarkGpu(Miner& miner) {
	const Header fixed{benchmarkHeader(0)};
	for (const auto start : {0U, 0xffffe000U}) {
		constexpr std::uint32_t count{4096}, threshold{0x08000000U};
		auto actual{miner.scan(fixed, start, count, threshold)};
		std::sort(actual.begin(), actual.end(), [](const auto& a, const auto& b) { return a.nonce < b.nonce; });
		std::vector<Sha256tCandidate> expected;
		for (std::uint32_t i{}; i < count; ++i) {
			Header header{fixed};
			const auto nonce{start + i};
			for (unsigned j{}; j < 4; ++j) header[76 + j] = static_cast<std::uint8_t>(nonce >> (j * 8));
			const auto hash{sha256t(header)};
			const std::uint32_t word{(static_cast<std::uint32_t>(hash[0]) << 24) |
				(static_cast<std::uint32_t>(hash[1]) << 16) | (static_cast<std::uint32_t>(hash[2]) << 8) | hash[3]};
			if (word >= kSha256tCandidateFloor && word < threshold) expected.push_back({nonce, word});
		}
		if (actual.size() != expected.size()) throw std::runtime_error("Benchmark GPU candidate count differs from CPU.");
		for (std::size_t i{}; i < actual.size(); ++i)
			if (actual[i].nonce != expected[i].nonce || actual[i].sha_word != expected[i].sha_word)
				throw std::runtime_error("Benchmark GPU candidate differs from CPU.");
	}
}

std::uint32_t launchSetting(const char* name, std::uint32_t fallback) {
	const auto text{environmentValue(name)};
	if (text.empty()) return fallback;
	std::uint32_t value{};
	const auto result{std::from_chars(text.data(), text.data() + text.size(), value)};
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
		throw std::invalid_argument(std::string{name} + " must be an integer.");
	return value;
}

} // namespace

Header benchmarkFixtureHeader(std::uint64_t epoch) { return benchmarkHeader(epoch); }
void verifyCudaBenchmarkWorkload(CudaSha256tMiner& miner) { verifyBenchmarkGpu(miner); }
void verifyCpuBenchmarkWorkload() { verifyBenchmarkCpu(); }

static int runEngine(std::string_view payout_address, std::string_view thread_setting,
			 std::string_view node_setting, const BenchmarkOptions* benchmark = nullptr,
			 BenchmarkResult* benchmark_result = nullptr, const TuningControls& tuning = {}) {
	if (!benchmark) validateAddress(payout_address);
	if (benchmark && (!std::isfinite(benchmark->duration_seconds) || benchmark->duration_seconds < 1 ||
		benchmark->duration_seconds > 86400 || !std::isfinite(benchmark->warmup_seconds) ||
		benchmark->warmup_seconds < 0 || benchmark->warmup_seconds > 3600))
		throw std::invalid_argument("Benchmark duration must be 1..86400 seconds and warmup 0..3600 seconds.");
	if (benchmark && (!std::isfinite(benchmark->initial_candidate_fraction) ||
		benchmark->initial_candidate_fraction < kInitialGpuCandidateFraction || benchmark->initial_candidate_fraction > .25))
		throw std::invalid_argument("Initial candidate fraction is outside the supported range.");
	const char* benchmark_setting{std::getenv("BRMINER_BENCHMARK_TELEMETRY")};
	const bool benchmark_telemetry{benchmark_setting && std::string_view{benchmark_setting} == "1"};
	std::string cuda_pipeline{environmentValue("BRMINER_EXPERIMENT_PIPELINE")};
	if (cuda_pipeline.empty()) cuda_pipeline = "sync";
	if (cuda_pipeline != "sync" && cuda_pipeline != "depth1" && cuda_pipeline != "depth2")
		throw std::invalid_argument("BRMINER_EXPERIMENT_PIPELINE must be sync, depth1, or depth2.");
	std::string affinity_policy{environmentValue("BRMINER_EXPERIMENT_AFFINITY")};
	if (affinity_policy.empty()) affinity_policy = "none";
	std::string queue_policy{environmentValue("BRMINER_EXPERIMENT_QUEUE")};
	if (queue_policy.empty()) queue_policy = "deque";
	std::size_t ring_capacity_candidates{};
	if (queue_policy == "l3ring-65536") ring_capacity_candidates = 65536;
	else if (queue_policy == "l3ring-262144") ring_capacity_candidates = 262144;
	else if (queue_policy == "l3ring-1048576") ring_capacity_candidates = 1048576;
	if (queue_policy != "deque" && queue_policy != "instrumented-deque" &&
		queue_policy != "deque-fastpath" && queue_policy != "instrumented-fastpath" &&
		ring_capacity_candidates == 0)
		throw std::invalid_argument(
			"BRMINER_EXPERIMENT_QUEUE must be deque, instrumented-deque, "
			"deque-fastpath, instrumented-fastpath, l3ring-65536, "
			"l3ring-262144, or l3ring-1048576.");
	const auto filter_policy{parseFilterPolicy(environmentValue("BRMINER_EXPERIMENT_FILTER"))};
	const brminer::PoolEndpoint endpoint{brminer::parsePoolEndpoint(node_setting)};
	const unsigned int hardware_threads{std::thread::hardware_concurrency()};
	std::uint32_t thread_count{thread_setting.empty()
		? std::max(1U, hardware_threads)
		: static_cast<std::uint32_t>(brminer::parseThreadCount(thread_setting))};
	auto cuda_batch_size{benchmark ? benchmark->cuda_batch_size :
		(tuning.cuda_batch_size ? *tuning.cuda_batch_size : launchSetting("BRMINER_CUDA_BATCH_SIZE", 4194304))};
	auto cuda_block_size{benchmark ? benchmark->cuda_block_size :
		(tuning.cuda_block_size ? *tuning.cuda_block_size : launchSetting("BRMINER_CUDA_BLOCK_SIZE", 256))};
	if (cuda_batch_size < 1048576 || cuda_batch_size > 67108864 || (cuda_batch_size & (cuda_batch_size - 1)))
		throw std::invalid_argument("CUDA batch size must be a power of two from 1048576 to 67108864.");
	if (cuda_block_size < 64 || cuda_block_size > 512 || (cuda_block_size & (cuda_block_size - 1)))
		throw std::invalid_argument("CUDA block size must be 64, 128, 256, or 512.");
	std::string requested_backend{benchmark ? benchmark->backend :
		(tuning.backend.empty() ? environmentValue("BRMINER_GPU_BACKEND") : tuning.backend)};
	if (requested_backend.empty()) requested_backend = "auto";
	if (requested_backend != "auto" && requested_backend != "cpu" && requested_backend != "cuda" && requested_backend != "opencl")
		throw std::invalid_argument("GPU backend must be auto, cpu, cuda, or opencl.");
	std::vector<std::string> gpu_devices;
	std::string gpu_detection_error;
        std::string nvrtc_dll;
        const auto explicit_compiler{benchmark ? benchmark->compiler_library : tuning.compiler_library};
        const auto compilers{!explicit_compiler.empty() ? std::vector<std::string>{explicit_compiler} : nvrtcLibraries()};
        bool use_cuda{};
        if (requested_backend != "cpu" && requested_backend != "opencl") for (const auto& compiler : compilers) {
                try {
                        gpu_devices = CudaSha256tMiner::devices(compiler);
                        use_cuda = !gpu_devices.empty();
                        if (use_cuda) { nvrtc_dll = compiler; gpu_detection_error.clear(); break; }
                }
                catch (const std::exception& error) {
                        gpu_detection_error = error.what();
                }
        }
        if (requested_backend == "cuda" && !use_cuda)
            throw std::runtime_error("CUDA backend unavailable: " + gpu_detection_error);
	if (cuda_pipeline != "sync" && !use_cuda)
		throw std::runtime_error("Experimental CUDA pipeline requires the CUDA backend.");
        if (!use_cuda && requested_backend != "cpu") {
                try {
                        gpu_devices = OpenClSha256tMiner::devices();
                }
                catch (const std::exception& error) {
                        if (!gpu_detection_error.empty()) gpu_detection_error += "; ";
                        gpu_detection_error += error.what();
                }
	}
	if (requested_backend == "opencl" && gpu_devices.empty()) throw std::runtime_error("OpenCL backend unavailable.");
	std::string tuning_message;
	double initial_candidate_fraction{benchmark ? benchmark->initial_candidate_fraction : kInitialGpuCandidateFraction};
	if (!benchmark && use_cuda) {
		CalibrationConstraints fixed;
		if (!thread_setting.empty()) fixed.threads = thread_count;
		if (tuning.cuda_batch_size || !environmentValue("BRMINER_CUDA_BATCH_SIZE").empty()) fixed.batch_size = cuda_batch_size;
		if (tuning.cuda_block_size || !environmentValue("BRMINER_CUDA_BLOCK_SIZE").empty()) fixed.block_size = cuda_block_size;
		try {
			const auto profile{resolveCalibration(nvrtc_dll, fixed, tuning)};
			thread_count = profile.configuration.threads;
			cuda_batch_size = profile.configuration.batch_size;
			cuda_block_size = profile.configuration.block_size;
			tuning_message = profile.message;
			initial_candidate_fraction = profile.initial_candidate_fraction;
		} catch (const CalibrationCancelled&) { throw; }
		catch (const std::exception& error) {
			tuning_message = std::string{"Calibration unavailable; using defaults/manual settings: "} + error.what();
		}
	}
	const auto affinity_plan{makeAffinityPlan(affinity_policy, thread_count, gpu_devices.size())};
	if (benchmark) {
		verifyBenchmarkCpu();
		benchmark_result->options = *benchmark;
		benchmark_result->options.threads = thread_count;
		benchmark_result->compiler_library = nvrtc_dll;
		benchmark_result->cuda_pipeline = cuda_pipeline;
		benchmark_result->affinity_policy = affinity_plan.policy;
		benchmark_result->affinity_assignments = affinity_plan.assignments;
		benchmark_result->queue_policy = queue_policy;
		benchmark_result->filter_policy = filter_policy.name;
		benchmark_result->gpu_names = gpu_devices;
		for (std::size_t i{}; i < gpu_devices.size(); ++i) {
			if (use_cuda) {
				CudaSha256tMiner verifier{i, nvrtc_dll, cuda_block_size};
				verifyBenchmarkGpu(verifier);
				benchmark_result->cuda_devices.push_back(verifier.identity());
			} else {
				OpenClSha256tMiner verifier{i};
				verifyBenchmarkGpu(verifier);
			}
		}
		benchmark_result->correctness_passed = true;
	}

	WinsockRuntime winsock;
	g_stop_requested = 0;
	const auto previous_sigint{std::signal(SIGINT, requestStop)};
	const auto previous_sigterm{std::signal(SIGTERM, requestStop)};
	MinerState state;
	state.benchmark = benchmark != nullptr;
	state.instrument_queue = queue_policy == "instrumented-deque" ||
		queue_policy == "instrumented-fastpath";
	state.queue_fastpath = queue_policy == "deque-fastpath" ||
		queue_policy == "instrumented-fastpath";
	state.queue_ring = ring_capacity_candidates != 0;
	if (state.queue_ring) {
		state.queue_limit = ring_capacity_candidates;
		state.gpu_candidate_ring =
			std::make_unique<BoundedMpmcRing<RingCandidateBatch>>(
				ring_capacity_candidates / kGpuCandidateTakeBatch);
	}
	state.gpu_candidate_floor.store(initial_candidate_fraction, std::memory_order_relaxed);
	const auto initial_threshold{candidateThreshold(initial_candidate_fraction)};
	state.gpu_candidate_threshold.store(initial_threshold, std::memory_order_relaxed);
	state.filter_minimum_threshold.store(initial_threshold, std::memory_order_relaxed);
	state.filter_maximum_threshold.store(initial_threshold, std::memory_order_relaxed);
	state.gpu_error = gpu_detection_error;
	const std::string processor{cpuDescription()};
	std::string gpu_name{"CPU only"};
	if (!gpu_devices.empty()) {
		gpu_name.clear();
		for (const auto& device : gpu_devices) {
			if (!gpu_name.empty()) gpu_name += ", ";
			gpu_name += device;
		}
	}
	const std::string backend{gpu_devices.empty() ? "CPU" : (use_cuda ? "CUDA" : "OpenCL")};
	state.console.start(backend, thread_count, benchmark_telemetry || benchmark, benchmark && benchmark->quiet);
	NvidiaMonitor monitor;
	if (benchmark_result) {
		benchmark_result->backend = backend;
		benchmark_result->devices = monitor.sample();
	}
	state.console.event(benchmark ? "Offline benchmark: deterministic work sequence." : "Warthog pool: " + endpoint.host + ":" + endpoint.port);
	state.console.event("Warthog GPU backend: " + backend);
	if (use_cuda) state.console.event("CUDA pipeline: " + cuda_pipeline + '.');
	state.console.event("Thread affinity: " + affinity_plan.policy + '.');
	if (state.instrument_queue) state.console.event("Candidate queue instrumentation enabled.");
	if (state.queue_fastpath) state.console.event("Candidate queue empty fast path enabled.");
	if (state.queue_ring) state.console.event(
		"Candidate queue L3 ring capacity: " + std::to_string(state.queue_limit) + '.');
	state.console.event("Candidate filter: " + filter_policy.name + '.');
	state.console.event("CPU: " + processor);
	state.console.event("GPU: " + gpu_name);
	if (!tuning_message.empty()) state.console.event(tuning_message);
	state.console.event("Janus combines CPU and GPU work. Estimated share time is not a countdown.");
	if (!gpu_detection_error.empty())
		state.console.event("GPU detection: " + gpu_detection_error);
	if (!gpu_devices.empty() && !monitor.available())
		state.console.event("GPU monitoring unavailable: " + monitor.unavailableReason());
	std::vector<std::thread> workers;
	const std::uint32_t cpu_mining_workers{gpu_devices.empty()
		? thread_count : 0U};
	const std::uint32_t worker_count{thread_count};
	workers.reserve(static_cast<std::size_t>(worker_count) + gpu_devices.size());
	for (std::uint32_t i{}; i < worker_count; ++i) {
		const bool verify_gpu_candidates{!gpu_devices.empty() && i >= cpu_mining_workers};
		workers.emplace_back([&, i, verify_gpu_candidates,
			affinity = affinity_plan.cpu_workers[i]] {
			try {
				applyCurrentThreadAffinity(affinity);
				miningWorker(state, verify_gpu_candidates);
			}
			catch (const std::exception& error) {
				recordWorkerFailure(state, "CPU worker " + std::to_string(i) +
					" stopped: " + error.what());
			}
		});
	}
	for (std::size_t i{}; i < gpu_devices.size(); ++i) {
		workers.emplace_back([&, i, affinity = affinity_plan.gpu_workers[i]] {
			try {
				applyCurrentThreadAffinity(affinity);
				gpuWorker(state, i, use_cuda, nvrtc_dll,
					cuda_batch_size, cuda_block_size, cuda_pipeline, filter_policy);
			}
			catch (const std::exception& error) {
				recordWorkerFailure(state, "GPU feeder " + std::to_string(i) +
					" stopped: " + error.what());
			}
		});
	}
	std::thread network;
	if (benchmark) network = std::thread{benchmarkWorker, std::ref(state)};
	else network = std::thread{networkWorker, std::ref(state), std::string{payout_address}, endpoint};
	if (benchmark) {
		const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{60}};
		while (state.gpu_workers_ready.load(std::memory_order_relaxed) < gpu_devices.size() &&
			!state.worker_failed.load(std::memory_order_relaxed) && !g_stop_requested &&
			std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds{20});
		if (state.gpu_workers_ready.load(std::memory_order_relaxed) < gpu_devices.size()) state.worker_failed.store(true);
	}

	std::uint64_t last_cpu_sha_hashes{};
	std::uint64_t last_gpu_sha_hashes{};
	std::uint64_t last_verus_hashes{};
	std::uint64_t last_gpu_candidates{};
	std::uint64_t last_gpu_scan_nanos{};
	std::uint64_t last_gpu_queue_wait_nanos{};
	if (benchmark) {
		last_cpu_sha_hashes = state.cpu_sha256t_hashes.load(std::memory_order_relaxed);
		last_gpu_sha_hashes = state.gpu_sha256t_hashes.load(std::memory_order_relaxed);
		last_verus_hashes = state.verushash_hashes.load(std::memory_order_relaxed);
		last_gpu_queue_wait_nanos = state.gpu_queue_wait_nanos.load(std::memory_order_relaxed);
	}
	auto last_sample{std::chrono::steady_clock::now()};
	const auto run_start{last_sample};
	double janus_work{};
	const auto sample_period{std::chrono::seconds{benchmark ? 1 : 10}};
	auto next_sample{last_sample + sample_period};
	while (!g_stop_requested) {
		while (!g_stop_requested && std::chrono::steady_clock::now() < next_sample) {
			std::this_thread::sleep_for(std::chrono::milliseconds{50});
		}
		if (g_stop_requested) break;
		const auto now{std::chrono::steady_clock::now()};
		const double elapsed{std::chrono::duration<double>(now - last_sample).count()};
		const auto cpu_sha_hashes{state.cpu_sha256t_hashes.load(std::memory_order_relaxed)};
		const auto gpu_sha_hashes{state.gpu_sha256t_hashes.load(std::memory_order_relaxed)};
		const auto verus_hashes{state.verushash_hashes.load(std::memory_order_relaxed)};
		const double cpu_sha_rate{static_cast<double>(cpu_sha_hashes - last_cpu_sha_hashes) / elapsed};
		const double gpu_sha_rate{static_cast<double>(gpu_sha_hashes - last_gpu_sha_hashes) / elapsed};
		const double sha_rate{cpu_sha_rate + gpu_sha_rate};
		const double verus_rate{static_cast<double>(verus_hashes - last_verus_hashes) / elapsed};
		const double janus_score{sha_rate == 0.0 ? 0.0 : (10.0 / 3.0) * sha_rate *
			(std::pow(0.005 + verus_rate / sha_rate, 0.3) - std::pow(0.005, 0.3))};
		if (std::isfinite(janus_score) && janus_score > 0.0) {
			state.suggested_share_difficulty.store(janus_score * kTargetShareIntervalSeconds,
				std::memory_order_relaxed);
		}
		const double uptime{std::chrono::duration<double>(now - run_start).count()};
		janus_work += janus_score * elapsed;
		MiningStats stats{uptime, janus_score, janus_work / uptime,
			gpu_sha_rate, cpu_sha_rate, verus_rate,
			state.share_difficulty.load(std::memory_order_relaxed),
			state.accepted_shares.load(std::memory_order_relaxed),
			state.rejected_shares.load(std::memory_order_relaxed),
			state.accepted_blocks.load(std::memory_order_relaxed)};
		stats.gpus = monitor.sample();
		if (!benchmark) state.console.update(stats);
		else {
			const double start{std::chrono::duration<double>(last_sample - run_start).count()};
			const double end{std::min(uptime, benchmark->warmup_seconds + benchmark->duration_seconds)};
			if (end > benchmark->warmup_seconds) {
				BenchmarkSample sample;
				sample.elapsed_s = end - benchmark->warmup_seconds;
				sample.interval_s = end - std::max(start, benchmark->warmup_seconds);
				sample.cpu_sha_hps = cpu_sha_rate;
				sample.gpu_sha_hps = gpu_sha_rate;
				sample.verus_hps = verus_rate;
				sample.janus_hps = janus_score;
				sample.gpus = stats.gpus;
				const auto wait{state.gpu_queue_wait_nanos.load(std::memory_order_relaxed)};
				sample.gpu_queue_wait_pct = static_cast<double>(wait - last_gpu_queue_wait_nanos) / (elapsed * 1e7);
				{
					std::lock_guard lock{state.mutex};
					sample.queue_depth = candidateQueueCount(state);
				}
				sample.gpu_filter_threshold =
					state.gpu_candidate_threshold.load(std::memory_order_relaxed);
				sample.gpu_filter_floor_fraction =
					state.gpu_candidate_floor.load(std::memory_order_relaxed);
				benchmark_result->samples.push_back(std::move(sample));
			}
			if (static_cast<int>(uptime) % 5 == 0) state.console.event(
				std::string{uptime <= benchmark->warmup_seconds ? "Warmup " : "Benchmark "} +
				formatUptime(uptime) + " | Janus " + formatHashrate(janus_score));
			last_gpu_queue_wait_nanos = state.gpu_queue_wait_nanos.load(std::memory_order_relaxed);
			if (uptime >= benchmark->warmup_seconds + benchmark->duration_seconds || state.worker_failed.load()) break;
		}
		if (benchmark_telemetry) {
			std::size_t queue_depth{};
			{
				std::lock_guard lock{state.mutex};
				queue_depth = candidateQueueCount(state);
			}
			const auto gpu_candidates{state.gpu_candidates_found.load(std::memory_order_relaxed)};
			const auto gpu_scan_nanos{state.gpu_scan_nanos.load(std::memory_order_relaxed)};
			const auto gpu_queue_wait_nanos{state.gpu_queue_wait_nanos.load(std::memory_order_relaxed)};
			std::ostringstream metrics;
			metrics << std::setprecision(12) << std::defaultfloat
					<< "BRMINER_METRICS elapsed_s="
					<< std::chrono::duration<double>(now - run_start).count()
					<< " cpu_sha_hps=" << cpu_sha_rate
					<< " gpu_sha_hps=" << gpu_sha_rate
					<< " verus_hps=" << verus_rate
					<< " janus_hps=" << janus_score
					<< " gpu_candidates_hps=" <<
						static_cast<double>(gpu_candidates - last_gpu_candidates) / elapsed
					<< " gpu_queue_depth=" << queue_depth
					<< " gpu_filter_threshold=" <<
						state.gpu_candidate_threshold.load(std::memory_order_relaxed)
					<< " gpu_filter_floor_fraction=" <<
						state.gpu_candidate_floor.load(std::memory_order_relaxed)
					<< " gpu_scan_busy_pct=" <<
						static_cast<double>(gpu_scan_nanos - last_gpu_scan_nanos) / (elapsed * 1e7)
					<< " gpu_queue_wait_pct=" <<
						static_cast<double>(gpu_queue_wait_nanos - last_gpu_queue_wait_nanos) / (elapsed * 1e7);
			state.console.diagnostic(metrics.str());
			last_gpu_candidates = gpu_candidates;
			last_gpu_scan_nanos = gpu_scan_nanos;
			last_gpu_queue_wait_nanos = gpu_queue_wait_nanos;
		}
		last_cpu_sha_hashes = cpu_sha_hashes;
		last_gpu_sha_hashes = gpu_sha_hashes;
		last_verus_hashes = verus_hashes;
		last_sample = now;
		next_sample += sample_period;
		while (next_sample <= now) next_sample += sample_period;
	}

	if (benchmark_result) benchmark_result->elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
	state.stop.store(true, std::memory_order_relaxed);
	state.changed.notify_all();
	for (auto& worker : workers) worker.join();
	network.join();
	if (benchmark_result) {
		benchmark_result->cuda_pipeline_stats = {
			state.cuda_batches_submitted.load(std::memory_order_relaxed),
			state.cuda_batches_completed.load(std::memory_order_relaxed),
			state.cuda_kernel_launches.load(std::memory_order_relaxed),
			state.cuda_overflow_retries.load(std::memory_order_relaxed),
			state.cuda_buffer_growths.load(std::memory_order_relaxed),
			state.cuda_host_wait_nanos.load(std::memory_order_relaxed)};
		benchmark_result->queue_stats = {
			state.queue_consumer_lock_wait_nanos.load(std::memory_order_relaxed),
			state.queue_consumer_lock_hold_nanos.load(std::memory_order_relaxed),
			state.queue_consumer_lock_acquisitions.load(std::memory_order_relaxed),
			state.queue_empty_checks.load(std::memory_order_relaxed),
			state.queue_fast_empty_checks.load(std::memory_order_relaxed),
			state.queue_batches_taken.load(std::memory_order_relaxed),
			state.queue_producer_lock_wait_nanos.load(std::memory_order_relaxed),
			state.queue_producer_lock_hold_nanos.load(std::memory_order_relaxed),
			state.queue_producer_lock_acquisitions.load(std::memory_order_relaxed),
			state.gpu_queue_wait_nanos.load(std::memory_order_relaxed),
			state.queue_high_water_candidates.load(std::memory_order_relaxed)};
		benchmark_result->filter_stats = {
			state.filter_adjustments.load(std::memory_order_relaxed),
			state.filter_minimum_threshold.load(std::memory_order_relaxed),
			state.filter_maximum_threshold.load(std::memory_order_relaxed)};
	}
	state.console.event("Mining stopped.");
	std::signal(SIGINT, previous_sigint);
	std::signal(SIGTERM, previous_sigterm);
	if (benchmark) {
		benchmark_result->cancelled = g_stop_requested != 0;
		summarizeBenchmark(*benchmark_result);
		if (state.worker_failed.load()) throw std::runtime_error("Benchmark worker failed: " + state.gpu_error);
	}
	return EXIT_SUCCESS;
}

int runMiner(std::string_view payout_address, std::string_view thread_setting, std::string_view node_setting,
	const TuningControls& tuning) {
	return runEngine(payout_address, thread_setting, node_setting, nullptr, nullptr, tuning);
}

BenchmarkResult runBenchmark(const BenchmarkOptions& options) {
	BenchmarkResult result;
	runEngine({}, options.threads ? std::to_string(options.threads) : "", {}, &options, &result);
	return result;
}

}  // namespace brminer::warthog
