#pragma once

#include "pow.hpp"

#include <randomx.h>

#include <memory>
#include <stdexcept>
#include <string_view>

namespace brminer {

class RandomXContext {
private:
    struct State {
        randomx_cache* cache = nullptr;

        ~State() {
            if (cache != nullptr) {
                randomx_release_cache(cache);
            }
        }
    };

public:
    class Worker {
    public:
        explicit Worker(std::shared_ptr<State> state) : state_(std::move(state)) {
            const auto flags = static_cast<randomx_flags>(
                static_cast<unsigned int>(randomx_get_flags()) |
                static_cast<unsigned int>(RANDOMX_FLAG_V2));
            vm_ = randomx_create_vm(flags, state_->cache, nullptr);
            if (vm_ == nullptr) {
                throw std::runtime_error("Could not create a RandomX v2 CPU worker.");
            }
        }

        ~Worker() {
            if (vm_ != nullptr) {
                randomx_destroy_vm(vm_);
            }
        }

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;

        Worker(Worker&& other) noexcept
            : state_(std::move(other.state_)), vm_(other.vm_) {
            other.vm_ = nullptr;
        }

        Worker& operator=(Worker&& other) noexcept {
            if (this != &other) {
                if (vm_ != nullptr) {
                    randomx_destroy_vm(vm_);
                }
                state_ = std::move(other.state_);
                vm_ = other.vm_;
                other.vm_ = nullptr;
            }
            return *this;
        }

        Hash256 hash(const BlockHeader& header) const {
            return hash(header.bytes.data(), header.bytes.size());
        }

        Hash256 hash(const void* input, std::size_t inputSize) const {
            Hash256 result{};
            randomx_calculate_hash(vm_, input, inputSize, result.data());
            return result;
        }

    private:
        std::shared_ptr<State> state_;
        randomx_vm* vm_ = nullptr;
    };

    explicit RandomXContext(std::string_view key)
        : RandomXContext(key.data(), key.size()) {}

    explicit RandomXContext(const Hash256& key)
        : RandomXContext(key.data(), key.size()) {}

private:
    RandomXContext(const void* key, std::size_t keySize)
        : state_(std::make_shared<State>()) {
        if (key == nullptr || keySize == 0) {
            throw std::invalid_argument("RandomX key must not be empty.");
        }
        randomx_flags flags = static_cast<randomx_flags>(
            static_cast<unsigned int>(randomx_get_flags()) |
            static_cast<unsigned int>(RANDOMX_FLAG_V2));
        flags = static_cast<randomx_flags>(
            static_cast<unsigned int>(flags) &
            ~static_cast<unsigned int>(RANDOMX_FLAG_FULL_MEM));
        state_->cache = randomx_alloc_cache(flags);
        if (state_->cache == nullptr) {
            throw std::runtime_error("Could not allocate the RandomX v2 cache.");
        }
        randomx_init_cache(state_->cache, key, keySize);
    }

public:
    Worker createWorker() const {
        return Worker(state_);
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace brminer
