#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#if defined(_MSC_VER)
 #pragma warning (push)
 #pragma warning (disable : 4324) // padding from alignas(64) is the point: no false sharing
#endif

namespace meridian
{

/** Bounded single-producer / single-consumer queue. Wait-free on both ends:
    push and pop never block, allocate or take locks, so either side may be the
    audio thread. Exactly one thread may push and exactly one may pop. */
template <typename T, size_t Capacity>
class SpscQueue
{
    static_assert (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push (const T& value) noexcept
    {
        const auto tail = writeIndex.load (std::memory_order_relaxed);
        const auto head = readIndex.load (std::memory_order_acquire);
        if (tail - head == Capacity)
            return false; // full

        slots[tail & (Capacity - 1)] = value;
        writeIndex.store (tail + 1, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const auto head = readIndex.load (std::memory_order_relaxed);
        const auto tail = writeIndex.load (std::memory_order_acquire);
        if (head == tail)
            return false; // empty

        out = slots[head & (Capacity - 1)];
        readIndex.store (head + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> slots {};
    alignas (64) std::atomic<size_t> writeIndex { 0 };
    alignas (64) std::atomic<size_t> readIndex { 0 };
};

} // namespace meridian

#if defined(_MSC_VER)
 #pragma warning (pop)
#endif
