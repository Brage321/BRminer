#include "bounded_mpmc_ring.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main() {
	using brminer::BoundedMpmcRing;
	BoundedMpmcRing<std::uint32_t> small{4};
	for (std::uint32_t value{}; value < 4; ++value)
		assert(small.tryEnqueue(std::move(value)));
	std::uint32_t overflow{4};
	assert(!small.tryEnqueue(std::move(overflow)));
	for (std::uint32_t expected{}; expected < 2; ++expected) {
		std::uint32_t actual{};
		assert(small.tryDequeue(actual) && actual == expected);
	}
	for (std::uint32_t value{4}; value < 6; ++value)
		assert(small.tryEnqueue(std::move(value)));
	for (std::uint32_t expected{2}; expected < 6; ++expected) {
		std::uint32_t actual{};
		assert(small.tryDequeue(actual) && actual == expected);
	}
	std::uint32_t empty{};
	assert(!small.tryDequeue(empty));

	constexpr std::uint32_t producer_count{2};
	constexpr std::uint32_t consumer_count{4};
	constexpr std::uint32_t values_per_producer{100000};
	constexpr std::uint32_t value_count{producer_count * values_per_producer};
	BoundedMpmcRing<std::uint32_t> ring{1024};
	auto seen{std::make_unique<std::atomic<std::uint8_t>[]>(value_count)};
	std::atomic<std::uint32_t> consumed{};
	std::atomic<bool> failed{};
	std::vector<std::thread> consumers;
	for (std::uint32_t consumer{}; consumer < consumer_count; ++consumer) {
		consumers.emplace_back([&] {
			while (consumed.load(std::memory_order_relaxed) < value_count) {
				std::uint32_t value{};
				if (!ring.tryDequeue(value)) {
					std::this_thread::yield();
					continue;
				}
				if (value >= value_count ||
					seen[value].fetch_add(1, std::memory_order_relaxed) != 0)
					failed.store(true, std::memory_order_relaxed);
				consumed.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	std::vector<std::thread> producers;
	for (std::uint32_t producer{}; producer < producer_count; ++producer) {
		producers.emplace_back([&, producer] {
			const std::uint32_t begin{producer * values_per_producer};
			for (std::uint32_t value{begin}; value < begin + values_per_producer; ++value) {
				std::uint32_t pending{value};
				while (!ring.tryEnqueue(std::move(pending))) std::this_thread::yield();
			}
		});
	}
	for (auto& producer : producers) producer.join();
	for (auto& consumer : consumers) consumer.join();
	assert(!failed.load(std::memory_order_relaxed));
	assert(consumed.load(std::memory_order_relaxed) == value_count);
	for (std::uint32_t value{}; value < value_count; ++value)
		assert(seen[value].load(std::memory_order_relaxed) == 1);
	std::cout << "Bounded MPMC ring passed.\n";
}
