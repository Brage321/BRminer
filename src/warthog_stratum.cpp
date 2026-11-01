#include "warthog_miner.hpp"

#include "config.hpp"
#include "warthog_gpu.hpp"
#include "warthog_pow.hpp"

#include <crypto/sha256.h>
#include <univalue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
#endif

namespace brminer::warthog {
namespace {

constexpr std::uint32_t kSubscribeId = 1;
constexpr std::uint32_t kAuthorizeId = 2;
constexpr std::uint32_t kSha256tCandidateFloor{0x0147ae14U};
constexpr double kInitialGpuCandidateFraction{0.00005};
constexpr double kTargetShareIntervalSeconds{60.0};
constexpr std::size_t kGpuCandidateQueueLimit{65536};
constexpr auto kGpuThresholdAdjustmentInterval{std::chrono::milliseconds{500}};

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

struct GpuCandidate {
	std::shared_ptr<const MiningJob> job;
	Sha256tCandidate candidate;
};

struct MinerState {
	std::mutex mutex;
	std::condition_variable changed;
	std::shared_ptr<const MiningJob> job;
	std::deque<GpuCandidate> gpu_candidates;
	std::atomic<std::uint32_t> gpu_candidate_threshold{candidateThreshold(kInitialGpuCandidateFraction)};
	std::uint64_t next_job_id{};
	std::atomic<std::uint64_t> next_nonce{0};
	std::atomic<std::uint64_t> cpu_sha256t_hashes{0};
	std::atomic<std::uint64_t> gpu_sha256t_hashes{0};
	std::atomic<std::uint64_t> verushash_hashes{0};
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
};

void recordSolution(MinerState& state, const std::shared_ptr<const MiningJob>& job,
					std::uint32_t nonce, const PowResult& result) {
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
	}
}

std::optional<GpuCandidate> takeGpuCandidate(MinerState& state) {
	std::lock_guard lock{state.mutex};
	while (!state.gpu_candidates.empty() &&
		   (!state.job || state.gpu_candidates.front().job->id != state.job->id)) {
		state.gpu_candidates.pop_front();
	}
	if (state.gpu_candidates.empty()) return std::nullopt;
	auto candidate{std::move(state.gpu_candidates.front())};
	state.gpu_candidates.pop_front();
	state.changed.notify_all();
	return candidate;
}

void verifyGpuCandidate(MinerState& state, GpuCandidate&& mined) {
	Header candidate_header{mined.job->header};
	candidate_header[76] = static_cast<std::uint8_t>(mined.candidate.nonce);
	candidate_header[77] = static_cast<std::uint8_t>(mined.candidate.nonce >> 8);
	candidate_header[78] = static_cast<std::uint8_t>(mined.candidate.nonce >> 16);
	candidate_header[79] = static_cast<std::uint8_t>(mined.candidate.nonce >> 24);
	const PowResult result{checkJanusHash(candidate_header, mined.candidate.hash,
									 mined.job->share_difficulty)};
	if (!result.sha256t_candidate) return;
	state.verushash_hashes.fetch_add(1, std::memory_order_relaxed);
	recordSolution(state, mined.job, mined.candidate.nonce, result);
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

#if defined(_WIN32)

class WinsockRuntime {
public:
	WinsockRuntime() {
		WSADATA data{};
		const int result{WSAStartup(MAKEWORD(2, 2), &data)};
		if (result != 0) throw std::runtime_error("Could not initialize Windows networking: " + std::to_string(result));
	}
	~WinsockRuntime() { WSACleanup(); }
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
		DWORD timeout{250};
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
		const int count{send(socket, data.data() + sent,
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
		const int received{recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0)};
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
	state.gpu_candidates.clear();
	state.solution.reset();
	state.solution_ready.store(false, std::memory_order_relaxed);
	state.job = std::make_shared<const MiningJob>(std::move(job));
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
				std::cout << "Pool accepted share." << std::endl;
				if (pending_block_candidate) {
					state.accepted_blocks.fetch_add(1, std::memory_order_relaxed);
				}
			}
			else {
				state.rejected_shares.fetch_add(1, std::memory_order_relaxed);
				std::cerr << "BRminer Warthog: pool rejected a share";
				if (!error.isNull()) std::cerr << ": " << error.write();
				std::cerr << std::endl;
			}
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
		if (latest_notify && subscribed && authorized) {
			publishNotify(state, *latest_notify, extra_nonce1, extra_nonce2_size,
						  extra_nonce_counter, difficulty);
		}
	}
	else if (method == "mining.notify") {
		latest_notify = parseNotify(params);
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
	while (!state.stop.load(std::memory_order_relaxed)) {
		std::shared_ptr<const MiningJob> job;
		{
			std::unique_lock lock{state.mutex};
			state.changed.wait(lock, [&] {
				return state.stop.load(std::memory_order_relaxed) ||
					   (state.job && !state.solution_ready.load(std::memory_order_relaxed) &&
						(state.next_nonce.load(std::memory_order_relaxed) <= UINT32_MAX ||
						 !state.gpu_candidates.empty()));
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
				if (auto gpu_candidate{takeGpuCandidate(state)}) {
					verifyGpuCandidate(state, std::move(*gpu_candidate));
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

void gpuWorker(MinerState& state, std::size_t device_index) {
	bool registered{};
	try {
		OpenClSha256tMiner miner{device_index};
		auto last_threshold_adjustment{std::chrono::steady_clock::now()};
		state.gpu_workers_ready.fetch_add(1, std::memory_order_relaxed);
		registered = true;
		while (!state.stop.load(std::memory_order_relaxed)) {
			std::shared_ptr<const MiningJob> job;
			{
				std::unique_lock lock{state.mutex};
				state.changed.wait(lock, [&] {
					return state.stop.load(std::memory_order_relaxed) ||
						   (state.job && !state.solution_ready.load(std::memory_order_relaxed) &&
							state.next_nonce.load(std::memory_order_relaxed) <= UINT32_MAX &&
									state.gpu_candidates.size() <= kGpuCandidateQueueLimit);
				});
				if (state.stop.load(std::memory_order_relaxed)) break;
				job = state.job;
			}
			constexpr std::uint64_t batch_size{65536};
			const std::uint64_t start{state.next_nonce.fetch_add(batch_size, std::memory_order_relaxed)};
			if (start > UINT32_MAX) continue;
			const auto count{static_cast<std::uint32_t>(std::min<std::uint64_t>(batch_size,
				static_cast<std::uint64_t>(UINT32_MAX) - start + 1))};
			const auto candidates{miner.scan(job->header, static_cast<std::uint32_t>(start), count,
				state.gpu_candidate_threshold.load(std::memory_order_relaxed))};
			state.gpu_sha256t_hashes.fetch_add(count, std::memory_order_relaxed);
			{
				std::lock_guard lock{state.mutex};
				if (!state.stop.load(std::memory_order_relaxed) && state.job &&
					state.job->id == job->id && !state.solution_ready.load(std::memory_order_relaxed)) {
					for (const auto& candidate : candidates) {
						state.gpu_candidates.push_back({job, candidate});
					}
					const auto now{std::chrono::steady_clock::now()};
					if (now - last_threshold_adjustment >= kGpuThresholdAdjustmentInterval) {
						const double fraction{static_cast<double>(
							state.gpu_candidate_threshold.load(std::memory_order_relaxed) -
							kSha256tCandidateFloor) /
							static_cast<double>(UINT32_MAX - kSha256tCandidateFloor)};
						const std::size_t queue_size{state.gpu_candidates.size()};
						const double adjusted_fraction{queue_size > kGpuCandidateQueueLimit / 4
							? std::max(0.00005, fraction * 0.8)
							: queue_size < kGpuCandidateQueueLimit / 8
								? std::min(0.5, fraction * 1.01)
								: fraction};
						state.gpu_candidate_threshold.store(candidateThreshold(adjusted_fraction),
							std::memory_order_relaxed);
						last_threshold_adjustment = now;
					}
				}
			}
			state.changed.notify_all();
		}
	}
	catch (const std::exception& error) {
		{
			std::lock_guard lock{state.mutex};
			if (!state.gpu_error.empty()) state.gpu_error += "; ";
			state.gpu_error += error.what();
		}
		state.changed.notify_all();
		std::cerr << "BRminer Warthog: GPU worker stopped: " << error.what() << std::endl;
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
				state.gpu_candidates.clear();
				state.solution.reset();
				state.solution_ready.store(false, std::memory_order_relaxed);
				state.changed.notify_all();
			}
			const auto now{std::chrono::steady_clock::now()};
			if (last_error != error.what() || now - last_error_time >= std::chrono::seconds{10}) {
				std::cerr << "BRminer Warthog: pool connection error (" << endpoint_name << "): "
					<< error.what() << std::endl;
				last_error = error.what();
				last_error_time = now;
			}
			std::this_thread::sleep_for(std::chrono::seconds{2});
		}
	}
}

#else

void miningWorker(MinerState&, bool) {}
void gpuWorker(MinerState&, std::size_t) {
	throw std::runtime_error("Warthog GPU mining currently requires Windows OpenCL.");
}
void networkWorker(MinerState&, std::string, brminer::PoolEndpoint) {
	throw std::runtime_error("Warthog Stratum mining currently requires Windows Winsock.");
}

#endif

}  // namespace

int runMiner(std::string_view payout_address, std::string_view thread_setting,
			 std::string_view node_setting) {
	validateAddress(payout_address);
	const brminer::PoolEndpoint endpoint{brminer::parsePoolEndpoint(node_setting)};
	const unsigned int hardware_threads{std::thread::hardware_concurrency()};
	const std::uint32_t thread_count{thread_setting.empty()
		? std::max(1U, hardware_threads)
		: static_cast<std::uint32_t>(brminer::parseThreadCount(thread_setting))};
	std::vector<std::string> gpu_devices;
	std::string gpu_detection_error;
	try {
		gpu_devices = OpenClSha256tMiner::devices();
	}
	catch (const std::exception& error) {
		gpu_detection_error = error.what();
	}

#if defined(_WIN32)
	WinsockRuntime winsock;
#endif
	g_stop_requested = 0;
	const auto previous_sigint{std::signal(SIGINT, requestStop)};
	const auto previous_sigterm{std::signal(SIGTERM, requestStop)};
	MinerState state;
	std::signal(SIGTERM, previous_sigterm);
	state.gpu_error = gpu_detection_error;
	std::vector<std::thread> workers;
	const std::uint32_t cpu_mining_workers{gpu_devices.empty()
		? thread_count
		: std::max(1U, (thread_count + 3) / 4)};
	const std::uint32_t worker_count{thread_count +
		(static_cast<std::uint32_t>(gpu_devices.size()) != 0 && cpu_mining_workers == thread_count ? 1U : 0U)};
	workers.reserve(static_cast<std::size_t>(worker_count) + gpu_devices.size());
	for (std::uint32_t i{}; i < worker_count; ++i) {
		const bool verify_gpu_candidates{!gpu_devices.empty() && i >= cpu_mining_workers};
		workers.emplace_back(miningWorker, std::ref(state), verify_gpu_candidates);
	}
	for (std::size_t i{}; i < gpu_devices.size(); ++i) {
		workers.emplace_back(gpuWorker, std::ref(state), i);
	}
	std::thread network{networkWorker, std::ref(state), std::string{payout_address}, endpoint};

	std::cout << "Warthog pool: " << endpoint.host << ":" << endpoint.port << std::endl;
	if (!gpu_detection_error.empty()) {
		std::cerr << "BRminer Warthog: GPU detection failed: " << gpu_detection_error << std::endl;
	}
	const std::string processor{processorDescription()};
	std::string gpu_name{"CPU only"};
	if (!gpu_devices.empty()) {
		gpu_name.clear();
		for (const auto& device : gpu_devices) {
			if (!gpu_name.empty()) gpu_name += ", ";
			gpu_name += device;
		}
	}
	const int cpu_column_width{std::max<int>(12, static_cast<int>(processor.size()))};
	const int gpu_column_width{std::max<int>(12, static_cast<int>(gpu_name.size()))};
	constexpr int hashrate_column_width{18};
	constexpr int difficulty_column_width{16};
	constexpr int interval_column_width{15};
	constexpr int share_column_width{17};
	constexpr int blocks_column_width{12};
	std::cout << std::left << std::setw(cpu_column_width) << "CPU NAME"
			  << " | " << std::setw(gpu_column_width) << "GPU NAME"
			  << " | " << std::right << std::setw(hashrate_column_width) << "HASHRATE"
			  << " | " << std::setw(difficulty_column_width) << "POOL DIFFICULTY"
			  << " | " << std::setw(interval_column_width) << "AVG SHARE TIME"
			  << " | " << std::setw(share_column_width) << "SHARES ACCEPTED"
			  << " | " << std::setw(share_column_width) << "SHARES REJECTED"
			  << " | " << std::setw(blocks_column_width) << "BLOCKS FOUND" << std::endl;
	std::uint64_t last_cpu_sha_hashes{};
	std::uint64_t last_gpu_sha_hashes{};
	std::uint64_t last_verus_hashes{};
	auto last_sample{std::chrono::steady_clock::now()};
	auto next_sample{last_sample + std::chrono::seconds{10}};
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
		double displayed_rate{sha_rate};
		const char* rate_unit{"H/s"};
		if (sha_rate >= 1'000'000'000'000.0) {
			displayed_rate /= 1'000'000'000'000.0;
			rate_unit = "TH/s";
		}
		else if (sha_rate >= 1'000'000'000.0) {
			displayed_rate /= 1'000'000'000.0;
			rate_unit = "GH/s";
		}
		else if (sha_rate >= 1'000'000.0) {
			displayed_rate /= 1'000'000.0;
			rate_unit = "MH/s";
		}
		else if (sha_rate >= 1'000.0) {
			displayed_rate /= 1'000.0;
			rate_unit = "kH/s";
		}
		std::ostringstream hashrate;
		hashrate << std::setprecision(3) << std::defaultfloat << displayed_rate << ' ' << rate_unit;
		const double pool_difficulty{state.share_difficulty.load(std::memory_order_relaxed)};
		std::ostringstream difficulty_text;
		if (pool_difficulty > 0.0) difficulty_text << std::fixed << std::setprecision(0) << pool_difficulty;
		else difficulty_text << "waiting";
		std::ostringstream interval_text;
		if (pool_difficulty > 0.0 && std::isfinite(janus_score) && janus_score > 0.0) {
			const double average_seconds{pool_difficulty / janus_score};
			interval_text << '~' << std::setprecision(2) << std::defaultfloat;
			if (average_seconds >= 3600.0) interval_text << average_seconds / 3600.0 << " h";
			else if (average_seconds >= 60.0) interval_text << average_seconds / 60.0 << " min";
			else interval_text << average_seconds << " s";
		}
		else interval_text << "waiting";
		std::cout << std::left << std::setw(cpu_column_width) << processor
				  << " | " << std::setw(gpu_column_width) << gpu_name
				  << " | " << std::right << std::setw(hashrate_column_width) << hashrate.str()
				  << " | " << std::setw(difficulty_column_width) << difficulty_text.str()
				  << " | " << std::setw(interval_column_width) << interval_text.str()
				  << " | " << std::setw(share_column_width)
				  << state.accepted_shares.load(std::memory_order_relaxed)
				  << " | " << std::setw(share_column_width)
				  << state.rejected_shares.load(std::memory_order_relaxed)
				  << " | " << std::setw(blocks_column_width)
				  << state.accepted_blocks.load(std::memory_order_relaxed) << std::endl;
		last_cpu_sha_hashes = cpu_sha_hashes;
		last_gpu_sha_hashes = gpu_sha_hashes;
		last_verus_hashes = verus_hashes;
		last_sample = now;
		next_sample += std::chrono::seconds{10};
		while (next_sample <= now) next_sample += std::chrono::seconds{10};
	}

	state.stop.store(true, std::memory_order_relaxed);
	state.changed.notify_all();
	for (auto& worker : workers) worker.join();
	network.join();
	std::signal(SIGINT, previous_sigint);
	std::signal(SIGTERM, previous_sigterm);
	return EXIT_SUCCESS;
}

}  // namespace brminer::warthog
