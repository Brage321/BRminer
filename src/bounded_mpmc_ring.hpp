#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace brminer {

// Dmitry Vyukov's bounded sequence-number queue, adapted to a runtime
// power-of-two capacity. The queue owns fixed storage and performs no
// allocation after construction.
template <typename T>
class BoundedMpmcRing {
public:
	explicit BoundedMpmcRing(std::size_t capacity)
		: capacity_{checkedCapacity(capacity)}, mask_{capacity_ - 1},
		  cells_{std::make_unique<Cell[]>(capacity_)} {
		for (std::size_t i{}; i < capacity; ++i)
			cells_[i].sequence.store(i, std::memory_order_relaxed);
	}

	BoundedMpmcRing(const BoundedMpmcRing&) = delete;
	BoundedMpmcRing& operator=(const BoundedMpmcRing&) = delete;

	std::size_t capacity() const noexcept { return capacity_; }

	bool tryEnqueue(T&& value) noexcept {
		return tryEnqueueWith(
			[&value](T& slot) noexcept { slot = std::move(value); },
			[]() noexcept {});
	}

	// fill runs after the slot is claimed. before_publish then runs after the
	// value is populated but before a consumer can observe it. Both must be
	// noexcept.
	template <typename Fill, typename BeforePublish>
	bool tryEnqueueWith(Fill&& fill, BeforePublish&& before_publish) noexcept {
		Cell* cell{};
		std::size_t position{enqueue_position_.load(std::memory_order_relaxed)};
		for (;;) {
			cell = &cells_[position & mask_];
			const std::size_t sequence{cell->sequence.load(std::memory_order_acquire)};
			const auto difference{static_cast<std::intptr_t>(sequence) -
				static_cast<std::intptr_t>(position)};
			if (difference == 0) {
				if (enqueue_position_.compare_exchange_weak(
					position, position + 1, std::memory_order_relaxed)) break;
			}
			else if (difference < 0) return false;
			else position = enqueue_position_.load(std::memory_order_relaxed);
		}
		std::forward<Fill>(fill)(cell->value);
		std::forward<BeforePublish>(before_publish)();
		cell->sequence.store(position + 1, std::memory_order_release);
		return true;
	}

	bool tryDequeue(T& value) noexcept {
		return tryDequeueWith(
			[&value](T& slot) noexcept { value = std::move(slot); });
	}

	// read must be noexcept. It runs while the caller exclusively owns the
	// claimed slot and before that slot is released back to a producer.
	template <typename Read>
	bool tryDequeueWith(Read&& read) noexcept {
		Cell* cell{};
		std::size_t position{dequeue_position_.load(std::memory_order_relaxed)};
		for (;;) {
			cell = &cells_[position & mask_];
			const std::size_t sequence{cell->sequence.load(std::memory_order_acquire)};
			const auto difference{static_cast<std::intptr_t>(sequence) -
				static_cast<std::intptr_t>(position + 1)};
			if (difference == 0) {
				if (dequeue_position_.compare_exchange_weak(
					position, position + 1, std::memory_order_relaxed)) break;
			}
			else if (difference < 0) return false;
			else position = dequeue_position_.load(std::memory_order_relaxed);
		}
		std::forward<Read>(read)(cell->value);
		cell->sequence.store(position + capacity_, std::memory_order_release);
		return true;
	}

private:
	static std::size_t checkedCapacity(std::size_t capacity) {
		if (capacity < 2 || (capacity & (capacity - 1)) != 0)
			throw std::invalid_argument("BoundedMpmcRing capacity must be a power of two");
		return capacity;
	}

	struct alignas(64) Cell {
		std::atomic<std::size_t> sequence{};
		T value{};
	};

	const std::size_t capacity_;
	const std::size_t mask_;
	std::unique_ptr<Cell[]> cells_;
	alignas(64) std::atomic<std::size_t> enqueue_position_{};
	alignas(64) std::atomic<std::size_t> dequeue_position_{};
};

}  // namespace brminer
