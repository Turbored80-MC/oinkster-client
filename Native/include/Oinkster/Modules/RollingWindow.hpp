#pragma once

#include "Oinkster/Events.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace Oinkster::Modules {

// Fixed-capacity FIFO. Pushing past capacity drops the oldest element, so
// memory is bounded no matter how fast samples arrive.
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : storage_(std::max<std::size_t>(capacity, 1)) {}

    void Push(T value) {
        storage_[(start_ + size_) % storage_.size()] = std::move(value);
        if (size_ < storage_.size()) ++size_;
        else start_ = (start_ + 1) % storage_.size();
    }
    void Clear() noexcept { start_ = 0; size_ = 0; }
    void PopFront() noexcept { if (size_ > 0) { start_ = (start_ + 1) % storage_.size(); --size_; } }

    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] std::size_t Capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }
    // 0 = oldest.
    [[nodiscard]] const T& At(std::size_t index) const { return storage_[(start_ + index) % storage_.size()]; }
    [[nodiscard]] const T& Front() const { return At(0); }
    [[nodiscard]] const T& Back() const { return At(size_ - 1); }

private:
    std::vector<T> storage_;
    std::size_t start_{};
    std::size_t size_{};
};

// Counts discrete events inside a trailing time window (e.g. clicks in the
// last second). Bounded: an absurd event rate cannot grow memory, it only
// saturates the count at the capacity.
class EventRateWindow {
public:
    explicit EventRateWindow(std::size_t capacity = 128) : events_(capacity) {}

    void Add(SteadyTime at) { events_.Push(at); }
    void Clear() noexcept { events_.Clear(); }

    // Events in (now - window, now].
    [[nodiscard]] std::size_t Count(SteadyTime now, SteadyClock::duration window) {
        while (!events_.Empty() && now - events_.Front() >= window) events_.PopFront();
        std::size_t count{};
        for (std::size_t index = 0; index < events_.Size(); ++index) {
            if (events_.At(index) <= now) ++count;
        }
        return count;
    }

private:
    RingBuffer<SteadyTime> events_;
};

}
