#include "connectcoin_pool.hpp"

#include "config.hpp"

#include <arith_uint256.h>
#include <chainparams.h>
#include <addresstype.h>
#include <crypto/sha256.h>
#include <key_io.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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

namespace brminer::connectcoin_pool {
namespace {

constexpr char kPoolHost[] = "pool.meshpool.net";
constexpr char kPoolPort[] = "3357";
constexpr std::uint32_t kSubscribeId{1};
constexpr std::uint32_t kAuthorizeId{2};
volatile std::sig_atomic_t g_stop_requested{0};

void requestStop(int) { g_stop_requested = 1; }

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

void validatePayoutAddress(std::string_view address) {
	if (address.empty() || address.size() > 160) {
		throw std::invalid_argument("BRMINER_PAYOUT_ADDRESS must be a valid ConnectCoin address.");
	}
	const CTxDestination destination{DecodeDestination(std::string{address})};
	const CScript script{GetScriptForDestination(destination)};
	const CTxOut output{0, script};
	if (!IsValidDestination(destination) || output.GetType() != TxOutputType::P2PK ||
		!output.GetP2PKPubKey()) {
		throw std::invalid_argument("BRMINER_PAYOUT_ADDRESS must be a valid ConnectCoin type-1 P2PK address.");
	}
}

std::uint32_t parseHex32(std::string_view text) {
	if (text.size() != 8) throw std::runtime_error("Pool sent an invalid 32-bit hexadecimal field.");
	std::uint32_t value{};
	for (const char digit : text) {
		value <<= 4;
		if (digit >= '0' && digit <= '9') value |= static_cast<std::uint32_t>(digit - '0');
		else if (digit >= 'a' && digit <= 'f') value |= static_cast<std::uint32_t>(digit - 'a' + 10);
		else if (digit >= 'A' && digit <= 'F') value |= static_cast<std::uint32_t>(digit - 'A' + 10);
		else throw std::runtime_error("Pool sent a non-hexadecimal 32-bit field.");
	}
	return value;
}

std::vector<std::uint8_t> decodeHex(std::string_view text) {
	if ((text.size() & 1U) != 0) throw std::runtime_error("Pool sent an odd-length hexadecimal field.");
	const auto nibble = [](char value) -> std::uint8_t {
		if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
		if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
		if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
		throw std::runtime_error("Pool sent a non-hexadecimal field.");
	};
	std::vector<std::uint8_t> bytes(text.size() / 2);
	for (std::size_t i{}; i < bytes.size(); ++i) {
		bytes[i] = static_cast<std::uint8_t>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
	}
	return bytes;
}

std::string encodeHex(const std::uint8_t* bytes, std::size_t size) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(size * 2, '0');
	for (std::size_t i{}; i < size; ++i) {
		result[i * 2] = digits[bytes[i] >> 4];
		result[i * 2 + 1] = digits[bytes[i] & 0x0f];
	}
	return result;
}

uint256 doubleSha256(const std::vector<std::uint8_t>& bytes) {
	std::array<std::uint8_t, CSHA256::OUTPUT_SIZE> first{};
	CSHA256{}.Write(bytes.data(), bytes.size()).Finalize(first.data());
	uint256 result;
	CSHA256{}.Write(first.data(), first.size()).Finalize(result.begin());
	return result;
}

arith_uint256 maximumTarget() {
	return arith_uint256{~arith_uint256{0}};
}

uint256 parseHash(std::string_view text) {
	const auto hash{uint256::FromHex(text)};
	if (!hash) throw std::runtime_error("Pool sent an invalid 256-bit hexadecimal field.");
	return *hash;
}

uint256 parseStratumPreviousHash(std::string_view text) {
	if (text.size() != 64) throw std::runtime_error("MeshPool sent an invalid Stratum previous-block hash.");
	std::vector<std::uint8_t> bytes{decodeHex(text)};
	for (std::size_t offset{}; offset < bytes.size(); offset += 4) {
		std::reverse(bytes.begin() + offset, bytes.begin() + offset + 4);
	}
	uint256 result;
	std::copy(bytes.begin(), bytes.end(), result.begin());
	return result;
}

uint256 parseStratumRandomXSeed(std::string_view text) {
	if (text.size() != 64) throw std::runtime_error("MeshPool sent an invalid RandomX seed.");
	const std::vector<std::uint8_t> bytes{decodeHex(text)};
	uint256 result;
	std::copy(bytes.begin(), bytes.end(), result.begin());
	return result;
}

arith_uint256 shareTarget(double difficulty) {
	if (!std::isfinite(difficulty) || difficulty <= 0.0) {
		throw std::runtime_error("Pool sent a non-positive or invalid share difficulty.");
	}
	int exponent{};
	const double fraction{std::frexp(difficulty, &exponent)};
	const std::uint64_t mantissa{static_cast<std::uint64_t>(std::ldexp(fraction, 53))};
	if (mantissa == 0) throw std::runtime_error("Pool share difficulty is too small to represent.");

	arith_uint256 diff_one;
	diff_one.SetCompact(0x1d00ffff);
	const arith_uint256 divisor{mantissa};
	const arith_uint256 scale{arith_uint256{1} << 53};
	const arith_uint256 quotient{diff_one / divisor};
	const arith_uint256 remainder{diff_one - quotient * divisor};
	arith_uint256 target{quotient * scale + (remainder * scale) / divisor};
	const arith_uint256 maximum{maximumTarget()};
	if (exponent < 0) {
		const unsigned int shift{static_cast<unsigned int>(-exponent)};
		if (shift >= 256 || target > (maximum >> shift)) return maximum;
		target <<= shift;
	} else if (exponent >= 256) {
		return arith_uint256{};
	} else {
		target >>= static_cast<unsigned int>(exponent);
	}
	return target;
}

struct Notify {
	std::string job_id;
	uint256 previous_hash;
	std::string coinbase1;
	std::string coinbase2;
	std::vector<uint256> merkle_branches;
	std::uint32_t version{};
	std::uint32_t bits{};
	std::uint32_t timestamp{};
	uint256 randomx_seed;
	bool clean{};
};

struct MiningJob {
	CBlockHeader header;
	uint256 randomx_seed;
	arith_uint256 share_target;
	arith_uint256 block_target;
	std::string pool_job_id;
	std::vector<std::uint8_t> extra_nonce2;
	std::uint32_t timestamp{};
	std::uint64_t id{};
};

struct MinedShare {
	std::string pool_job_id;
	std::vector<std::uint8_t> extra_nonce2;
	std::uint32_t timestamp{};
	std::uint32_t nonce{};
	bool block_candidate{};
};

struct MinerState {
	std::mutex mutex;
	std::condition_variable changed;
	std::shared_ptr<const MiningJob> job;
	std::uint64_t next_job_id{};
	std::atomic<std::uint64_t> next_nonce{0};
	std::atomic<std::uint64_t> hashes{0};
	std::atomic<std::uint64_t> accepted_shares{0};
	std::atomic<std::uint64_t> rejected_shares{0};
	std::atomic<std::uint64_t> accepted_blocks{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> connected{false};
	std::atomic<bool> solution_ready{false};
	std::optional<MinedShare> solution;
};

std::vector<std::uint8_t> makeExtraNonce2(std::uint64_t counter, std::uint32_t size) {
	if (size < 1 || size > sizeof(counter)) {
		throw std::runtime_error("Pool extranonce2 size must be from 1 to 8 bytes.");
	}
	std::array<std::uint8_t, sizeof(counter)> encoded{};
	for (std::size_t i{}; i < encoded.size(); ++i) {
		encoded[encoded.size() - i - 1] = static_cast<std::uint8_t>(counter >> (i * 8));
	}
	return {encoded.end() - size, encoded.end()};
}

Notify parseNotify(const UniValue& params) {
	if (!params.isArray() || params.size() != 10) {
		throw std::runtime_error("MeshPool sent a mining.notify message with an unexpected parameter count.");
	}
	for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 7U, 9U}) {
		if (!params[index].isStr()) throw std::runtime_error("MeshPool sent a malformed mining.notify field.");
	}
	if (!params[8].isBool()) throw std::runtime_error("MeshPool sent an invalid clean-jobs flag.");

	Notify notify;
	notify.job_id = params[0].get_str();
	notify.previous_hash = parseStratumPreviousHash(params[1].get_str());
	notify.coinbase1 = params[2].get_str();
	notify.coinbase2 = params[3].get_str();
	const UniValue& branches{params[4]};
	if (!branches.isArray()) throw std::runtime_error("MeshPool sent malformed Merkle branches.");
	notify.merkle_branches.reserve(branches.size());
	for (const UniValue& branch : branches.getValues()) {
		if (!branch.isStr()) throw std::runtime_error("MeshPool sent a malformed Merkle branch.");
		notify.merkle_branches.push_back(parseHash(branch.get_str()));
	}
	notify.version = parseHex32(params[5].get_str());
	notify.bits = parseHex32(params[6].get_str());
	notify.timestamp = parseHex32(params[7].get_str());
	notify.clean = params[8].get_bool();
	notify.randomx_seed = parseStratumRandomXSeed(params[9].get_str());
	return notify;
}

MiningJob buildJob(const Notify& notify, const std::vector<std::uint8_t>& extra_nonce1,
				   std::uint32_t extra_nonce2_size, std::uint64_t extra_nonce_counter,
				   double difficulty, std::uint64_t job_id) {
	MiningJob job;
	job.pool_job_id = notify.job_id;
	job.extra_nonce2 = makeExtraNonce2(extra_nonce_counter, extra_nonce2_size);
	job.timestamp = notify.timestamp;
	job.id = job_id;
	job.randomx_seed = notify.randomx_seed;
	job.share_target = shareTarget(difficulty);
	const auto block_target{DeriveTarget(notify.bits, Params().GetConsensus().powLimit)};
	if (!block_target) throw std::runtime_error("MeshPool sent an invalid ConnectCoin network target.");
	job.block_target = *block_target;

	std::vector<std::uint8_t> coinbase;
	const auto prefix{decodeHex(notify.coinbase1)};
	const auto suffix{decodeHex(notify.coinbase2)};
	coinbase.reserve(prefix.size() + extra_nonce1.size() + job.extra_nonce2.size() + suffix.size());
	coinbase.insert(coinbase.end(), prefix.begin(), prefix.end());
	coinbase.insert(coinbase.end(), extra_nonce1.begin(), extra_nonce1.end());
	coinbase.insert(coinbase.end(), job.extra_nonce2.begin(), job.extra_nonce2.end());
	coinbase.insert(coinbase.end(), suffix.begin(), suffix.end());
	uint256 merkle_root{doubleSha256(coinbase)};
	for (const uint256& branch : notify.merkle_branches) {
		std::vector<std::uint8_t> pair;
		pair.reserve(64);
		pair.insert(pair.end(), merkle_root.begin(), merkle_root.end());
		pair.insert(pair.end(), branch.begin(), branch.end());
		merkle_root = doubleSha256(pair);
	}

	job.header.nVersion = static_cast<std::int32_t>(notify.version);
	job.header.hashPrevBlock = notify.previous_hash;
	job.header.hashMerkleRoot = merkle_root;
	job.header.nTime = notify.timestamp;
	job.header.nBits = notify.bits;
	job.header.nNonce = 0;
	PrepareRandomXKey(job.randomx_seed, Params().GetConsensus());
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

SocketHandle connectPool() {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfo* addresses{};
	const int lookup{getaddrinfo(kPoolHost, kPoolPort, &hints, &addresses)};
	if (lookup != 0) throw std::runtime_error("Could not resolve MeshPool host (Winsock " + std::to_string(lookup) + ").");
	struct AddressList {
		addrinfo* value;
		~AddressList() { freeaddrinfo(value); }
	} address_list{addresses};

	int last_error{WSAECONNREFUSED};
	for (addrinfo* address{addresses}; address; address = address->ai_next) {
		SocketHandle socket{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
		if (!socket) { last_error = WSAGetLastError(); continue; }
		DWORD timeout{250};
		setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
				   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		if (::connect(socket.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) return socket;
		last_error = WSAGetLastError();
	}
	throw std::runtime_error("Could not connect to MeshPool at pool.meshpool.net:3357 (Winsock " +
							 std::to_string(last_error) + ").");
}

void sendAll(SOCKET socket, std::string_view data) {
	std::size_t sent{};
	while (sent < data.size()) {
		const int count{send(socket, data.data() + sent,
							 static_cast<int>(std::min<std::size_t>(data.size() - sent, INT_MAX)), 0)};
		if (count == SOCKET_ERROR || count == 0) {
			throw std::runtime_error("MeshPool Stratum send failed (Winsock " + std::to_string(WSAGetLastError()) + ").");
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
		if (buffer.size() > 1024 * 1024) throw std::runtime_error("MeshPool Stratum message exceeded the 1 MiB limit.");
		std::array<char, 4096> bytes{};
		const int received{recv(socket, bytes.data(), static_cast<int>(bytes.size()), 0)};
		if (received == 0) return ReadResult::closed;
		if (received == SOCKET_ERROR) {
			const int error{WSAGetLastError()};
			if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) return ReadResult::timeout;
			throw std::runtime_error("MeshPool Stratum receive failed (Winsock " + std::to_string(error) + ").");
		}
		buffer.append(bytes.data(), static_cast<std::size_t>(received));
	}
}

std::string stratumRequest(std::uint32_t id, std::string_view method, const UniValue& params) {
	UniValue message{UniValue::VOBJ};
	message.pushKV("id", static_cast<std::int64_t>(id));
	message.pushKV("method", std::string{method});
	message.pushKV("params", params);
	return message.write() + "\n";
}

std::string submitRequest(std::uint32_t id, std::string_view worker,
						  const MinedShare& share) {
	UniValue params{UniValue::VARR};
	params.push_back(std::string{worker});
	params.push_back(share.pool_job_id);
	params.push_back(encodeHex(share.extra_nonce2.data(), share.extra_nonce2.size()));
	std::array<std::uint8_t, 4> timestamp{
		static_cast<std::uint8_t>(share.timestamp >> 24),
		static_cast<std::uint8_t>(share.timestamp >> 16),
		static_cast<std::uint8_t>(share.timestamp >> 8),
		static_cast<std::uint8_t>(share.timestamp)};
	params.push_back(encodeHex(timestamp.data(), timestamp.size()));
	std::array<std::uint8_t, 4> nonce{
		static_cast<std::uint8_t>(share.nonce >> 24),
		static_cast<std::uint8_t>(share.nonce >> 16),
		static_cast<std::uint8_t>(share.nonce >> 8),
		static_cast<std::uint8_t>(share.nonce)};
	params.push_back(encodeHex(nonce.data(), nonce.size()));
	return stratumRequest(id, "mining.submit", params);
}

std::int64_t requireInteger(const UniValue& object, const char* key) {
	const UniValue& value{object[key]};
	if (!value.isNum()) throw std::runtime_error(std::string{"Pool response missing numeric field: "} + key);
	return value.getInt<std::int64_t>();
}

void publishJob(MinerState& state, const Notify& notify,
				const std::vector<std::uint8_t>& extra_nonce1, std::uint32_t extra_nonce2_size,
				std::uint64_t& extra_nonce_counter, double difficulty) {
	std::lock_guard lock{state.mutex};
	++extra_nonce_counter;
	MiningJob job{buildJob(notify, extra_nonce1, extra_nonce2_size,
						   extra_nonce_counter, difficulty, ++state.next_job_id)};
	state.next_nonce.store(0, std::memory_order_relaxed);
	state.solution.reset();
	state.solution_ready.store(false, std::memory_order_relaxed);
	state.job = std::make_shared<const MiningJob>(std::move(job));
	state.changed.notify_all();
}

void handleMessage(MinerState& state, const UniValue& message, bool& subscribed, bool& authorized,
				   std::vector<std::uint8_t>& extra_nonce1, std::uint32_t& extra_nonce2_size,
				   std::uint64_t& extra_nonce_counter, double& difficulty,
				   std::optional<Notify>& latest_notify, std::optional<std::uint32_t>& submit_id,
				   bool& pending_block_candidate) {
	if (!message.isObject()) throw std::runtime_error("MeshPool sent a non-object Stratum message.");
	const UniValue& id_value{message["id"]};
	if (!id_value.isNull()) {
		const std::int64_t id{requireInteger(message, "id")};
		const UniValue& error{message["error"]};
		const UniValue& result{message["result"]};
		if (submit_id && id == *submit_id) {
			if (error.isNull() && result.isBool() && result.get_bool()) {
				state.accepted_shares.fetch_add(1, std::memory_order_relaxed);
				std::cout << "MeshPool acknowledged submitted share; verify pool validation in the dashboard." << std::endl;
				if (pending_block_candidate) state.accepted_blocks.fetch_add(1, std::memory_order_relaxed);
			} else {
				state.rejected_shares.fetch_add(1, std::memory_order_relaxed);
				std::cerr << "BRminer ConnectCoin: MeshPool rejected a share";
				if (!error.isNull()) std::cerr << ": " << error.write();
				else if (!result.isNull()) std::cerr << ": result=" << result.write();
				else std::cerr << ": response had neither an error nor a result";
				std::cerr << std::endl;
			}
			submit_id.reset();
			pending_block_candidate = false;
			state.solution_ready.store(false, std::memory_order_relaxed);
			state.changed.notify_all();
			return;
		}
		if (!error.isNull()) throw std::runtime_error("MeshPool Stratum request failed: " + error.write());
		if (id == kSubscribeId) {
			if (!result.isArray() || result.size() < 3 || !result[1].isStr() || !result[2].isNum()) {
				throw std::runtime_error("MeshPool sent an invalid mining.subscribe response.");
			}
			extra_nonce1 = decodeHex(result[1].get_str());
			extra_nonce2_size = result[2].getInt<std::uint32_t>();
			if (extra_nonce2_size < 1 || extra_nonce2_size > 8) {
				throw std::runtime_error("MeshPool extranonce2 size must be from 1 to 8 bytes.");
			}
			subscribed = true;
			std::cout << "BRminer ConnectCoin: subscribed to MeshPool." << std::endl;
			if (authorized && latest_notify) {
				publishJob(state, *latest_notify, extra_nonce1, extra_nonce2_size, extra_nonce_counter, difficulty);
			}
		} else if (id == kAuthorizeId) {
			if (!result.isBool() || !result.get_bool()) {
				throw std::runtime_error("MeshPool rejected the ConnectCoin payout address.");
			}
			authorized = true;
			std::cout << "BRminer ConnectCoin: wallet authorized by MeshPool." << std::endl;
			if (subscribed && latest_notify) {
				publishJob(state, *latest_notify, extra_nonce1, extra_nonce2_size, extra_nonce_counter, difficulty);
			}
		}
		return;
	}

	const UniValue& method_value{message["method"]};
	const UniValue& params{message["params"]};
	if (!method_value.isStr() || !params.isArray()) {
		throw std::runtime_error("MeshPool sent a malformed Stratum notification.");
	}
	const std::string method{method_value.get_str()};
	if (method == "mining.set_difficulty") {
		if (params.size() != 1 || !params[0].isNum()) throw std::runtime_error("MeshPool sent an invalid share difficulty.");
		difficulty = params[0].get_real();
		if (!std::isfinite(difficulty) || difficulty <= 0.0) throw std::runtime_error("MeshPool sent an invalid share difficulty.");
		if (latest_notify && subscribed && authorized) {
			publishJob(state, *latest_notify, extra_nonce1, extra_nonce2_size, extra_nonce_counter, difficulty);
		}
	} else if (method == "mining.notify") {
		latest_notify = parseNotify(params);
		if (subscribed && authorized) {
			publishJob(state, *latest_notify, extra_nonce1, extra_nonce2_size, extra_nonce_counter, difficulty);
		}
	} else if (method == "mining.set_extranonce") {
		if (params.size() != 2 || !params[0].isStr() || !params[1].isNum()) {
			throw std::runtime_error("MeshPool sent an invalid extranonce update.");
		}
		extra_nonce1 = decodeHex(params[0].get_str());
		extra_nonce2_size = params[1].getInt<std::uint32_t>();
		if (extra_nonce2_size < 1 || extra_nonce2_size > 8) throw std::runtime_error("MeshPool extranonce2 size must be from 1 to 8 bytes.");
		if (latest_notify && subscribed && authorized) {
			publishJob(state, *latest_notify, extra_nonce1, extra_nonce2_size, extra_nonce_counter, difficulty);
		}
	}
}

void miningWorker(MinerState& state) {
	std::uint64_t seen_job{};
	while (!state.stop.load(std::memory_order_relaxed)) {
		std::shared_ptr<const MiningJob> job;
		{
			std::unique_lock lock{state.mutex};
			state.changed.wait(lock, [&] {
				return state.stop.load(std::memory_order_relaxed) ||
					   (state.job && !state.solution_ready.load(std::memory_order_relaxed) &&
						state.next_nonce.load(std::memory_order_relaxed) <= UINT32_MAX);
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
			const std::uint64_t nonce{state.next_nonce.fetch_add(1, std::memory_order_relaxed)};
			if (nonce > UINT32_MAX) break;
			CBlockHeader candidate{job->header};
			candidate.nNonce = static_cast<std::uint32_t>(nonce);
			state.hashes.fetch_add(1, std::memory_order_relaxed);
			const arith_uint256 hash{UintToArith256(GetPoWHash(
				candidate, job->randomx_seed, Params().GetConsensus(), /*secure_jit=*/false))};
			const bool block_candidate{hash <= job->block_target};
			if (!block_candidate && hash > job->share_target) continue;

			MinedShare share{job->pool_job_id, job->extra_nonce2, job->timestamp,
							 static_cast<std::uint32_t>(nonce), block_candidate};
			std::lock_guard lock{state.mutex};
			if (state.job && state.job->id == seen_job && !state.solution) {
				state.solution = std::move(share);
				state.solution_ready.store(true, std::memory_order_relaxed);
				state.changed.notify_all();
			}
			break;
		}
	}
}

void networkWorker(MinerState& state, std::string payout_address) {
	const std::string worker_name{payout_address + ".BRminer"};
	std::string last_error;
	auto last_error_time = std::chrono::steady_clock::time_point{};
	while (!state.stop.load(std::memory_order_relaxed)) {
		try {
			SocketHandle socket{connectPool()};
			state.connected.store(true, std::memory_order_relaxed);
			std::cout << "BRminer ConnectCoin: connected to MeshPool at " << kPoolHost << ':' << kPoolPort << std::endl;
			UniValue subscribe_params{UniValue::VARR};
			subscribe_params.push_back("BRminer/1.0");
			sendAll(socket.get(), stratumRequest(kSubscribeId, "mining.subscribe", subscribe_params));
			UniValue authorize_params{UniValue::VARR};
			authorize_params.push_back(worker_name);
			authorize_params.push_back("x");
			sendAll(socket.get(), stratumRequest(kAuthorizeId, "mining.authorize", authorize_params));

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
			std::string receive_buffer;
			while (!state.stop.load(std::memory_order_relaxed)) {
				if (!submit_id) {
					std::optional<MinedShare> share;
					{
						std::lock_guard lock{state.mutex};
						if (state.solution) {
							share = std::move(state.solution);
							state.solution.reset();
						}
					}
					if (share) {
						submit_id = next_id++;
						pending_block_candidate = share->block_candidate;
						sendAll(socket.get(), submitRequest(*submit_id, worker_name, *share));
					}
				}
				std::string line;
				const ReadResult result{readLine(socket.get(), receive_buffer, line)};
				if (result == ReadResult::closed) throw std::runtime_error("MeshPool closed the Stratum connection.");
				if (result == ReadResult::timeout) continue;
				UniValue message;
				if (!message.read(line)) throw std::runtime_error("MeshPool sent malformed Stratum JSON.");
				handleMessage(state, message, subscribed, authorized, extra_nonce1, extra_nonce2_size,
							  extra_nonce_counter, difficulty, latest_notify, submit_id, pending_block_candidate);
			}
			return;
		} catch (const std::exception& error) {
			state.connected.store(false, std::memory_order_relaxed);
			{
				std::lock_guard lock{state.mutex};
				state.job.reset();
				state.solution.reset();
				state.solution_ready.store(false, std::memory_order_relaxed);
				state.changed.notify_all();
			}
			const auto now{std::chrono::steady_clock::now()};
			if (last_error != error.what() || now - last_error_time >= std::chrono::seconds{10}) {
				std::cerr << "BRminer ConnectCoin: pool connection error: " << error.what() << std::endl;
				last_error = error.what();
				last_error_time = now;
			}
			std::this_thread::sleep_for(std::chrono::seconds{2});
		}
	}
}

#else

void miningWorker(MinerState&) {}
void networkWorker(MinerState&, std::string) {
	throw std::runtime_error("ConnectCoin pool mining currently requires Windows Winsock.");
}

#endif

}  // namespace

int runMiner(std::string_view payout_address, std::string_view thread_setting) {
#if !defined(_WIN32)
	(void)payout_address;
	(void)thread_setting;
	throw std::runtime_error("ConnectCoin pool mining currently requires Windows Winsock.");
#else
	SelectParams(ChainType::MAIN);
	validatePayoutAddress(payout_address);
	const unsigned int hardware_threads{std::thread::hardware_concurrency()};
	const std::uint32_t thread_count{thread_setting.empty()
		? std::max(1U, hardware_threads)
		: static_cast<std::uint32_t>(brminer::parseThreadCount(thread_setting))};

	WinsockRuntime winsock;
	g_stop_requested = 0;
	const auto previous_sigint{std::signal(SIGINT, requestStop)};
	const auto previous_sigterm{std::signal(SIGTERM, requestStop)};
	MinerState state;
	std::vector<std::thread> workers;
	workers.reserve(thread_count);
	for (std::uint32_t i{}; i < thread_count; ++i) workers.emplace_back(miningWorker, std::ref(state));
	std::thread network{networkWorker, std::ref(state), std::string{payout_address}};

	std::cout << "BRminer ConnectCoin pool mining for " << payout_address << " with "
			  << thread_count << " CPU worker(s). Pool: stratum+tcp://" << kPoolHost << ':'
			  << kPoolPort << ". Press Ctrl+C to stop." << std::endl;

	const std::string processor{processorDescription()};
	constexpr int cpu_column_width{70};
	constexpr int hashrate_column_width{18};
	constexpr int blocks_column_width{12};
	const std::string table_rule(cpu_column_width + hashrate_column_width + blocks_column_width + 6, '-');
	std::cout << table_rule << '\n'
			  << std::left << std::setw(cpu_column_width) << "CPU NAME"
			  << " | " << std::setw(hashrate_column_width) << "HASHRATE"
			  << " | " << std::setw(blocks_column_width) << "BLOCKS" << " |\n"
			  << table_rule << std::endl;

	auto last_sample{std::chrono::steady_clock::now()};
	std::uint64_t last_hashes{};
	while (!g_stop_requested) {
		std::this_thread::sleep_for(std::chrono::seconds{1});
		const auto now{std::chrono::steady_clock::now()};
		if (now - last_sample < std::chrono::seconds{10}) continue;
		const double elapsed{std::chrono::duration<double>(now - last_sample).count()};
		const std::uint64_t hashes{state.hashes.load(std::memory_order_relaxed)};
		std::ostringstream hashrate;
		hashrate << std::fixed << std::setprecision(1)
				 << static_cast<double>(hashes - last_hashes) / elapsed << " H/s";
		bool has_job{};
		{
			std::lock_guard lock{state.mutex};
			has_job = static_cast<bool>(state.job);
		}
		std::cout << std::left << std::setw(cpu_column_width) << processor
				  << " | " << std::setw(hashrate_column_width) << hashrate.str()
				  << " | " << std::setw(blocks_column_width)
				  << state.accepted_blocks.load(std::memory_order_relaxed) << " |\n";
		std::cout << "Core v1.0.3 | Acknowledged: "
				  << state.accepted_shares.load(std::memory_order_relaxed)
				  << " | RPC rejected: " << state.rejected_shares.load(std::memory_order_relaxed)
				  << " | Miner: " << (has_job ? "mining" : "waiting") << std::endl;
		last_hashes = hashes;
		last_sample = now;
	}

	state.stop.store(true, std::memory_order_relaxed);
	state.changed.notify_all();
	for (auto& worker : workers) worker.join();
	network.join();
	std::signal(SIGINT, previous_sigint);
	std::signal(SIGTERM, previous_sigterm);
	return EXIT_SUCCESS;
#endif
}

}  // namespace brminer::connectcoin_pool
