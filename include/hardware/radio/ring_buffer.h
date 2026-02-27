/**
 * @file ring_buffer.h
 * @brief Statically-allocated, ISR-safe ring buffer for the radio subsystem.
 *
 * The buffer is designed to be filled from an interrupt context (the
 * hardware RX callback) and drained from a FreeRTOS worker task.  It
 * uses a single-producer / single-consumer lock-free model based on
 * `volatile` head/tail indices – no mutex is required.
 *
 * @tparam T        Element type (must be trivially copyable).
 * @tparam Capacity Number of elements the buffer can hold (must be > 0).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hackos::radio {

template <typename T, size_t Capacity>
class RingBuffer
{
    static_assert(Capacity > 0U, "RingBuffer capacity must be > 0");
    static_assert(std::is_trivially_copyable<T>::value,
                  "RingBuffer elements must be trivially copyable");

public:
    RingBuffer() : head_(0U), tail_(0U) {}

    // ── Capacity queries ─────────────────────────────────────────────────

    /// @brief Maximum number of elements the buffer can hold.
    static constexpr size_t capacity() { return Capacity; }

    /// @brief Number of elements currently stored.
    size_t count() const
    {
        const size_t h = head_;
        const size_t t = tail_;
        return (h >= t) ? (h - t) : (Capacity + 1U - t + h);
    }

    /// @brief True when no elements are stored.
    bool empty() const { return head_ == tail_; }

    /// @brief True when no more elements can be pushed.
    bool full() const { return next(head_) == tail_; }

    // ── Producer API (call from ISR / single producer) ───────────────────

    /**
     * @brief Push an element into the buffer.
     * @return true on success, false if the buffer is full.
     */
    bool push(const T &item)
    {
        const size_t nxt = next(head_);
        if (nxt == tail_)
        {
            return false; // full
        }
        data_[head_] = item;
        head_ = nxt;
        return true;
    }

    // ── Consumer API (call from worker task / single consumer) ───────────

    /**
     * @brief Pop the oldest element from the buffer.
     * @param[out] item  Receives the popped element.
     * @return true on success, false if the buffer is empty.
     */
    bool pop(T &item)
    {
        if (head_ == tail_)
        {
            return false; // empty
        }
        item = data_[tail_];
        tail_ = next(tail_);
        return true;
    }

    /// @brief Discard all elements.
    void reset()
    {
        head_ = 0U;
        tail_ = 0U;
    }

private:
    /// @brief Advance an index with wrap-around.
    static size_t next(size_t idx) { return (idx + 1U) % (Capacity + 1U); }

    T data_[Capacity + 1U]; ///< One extra slot for full/empty disambiguation.
    volatile size_t head_;
    volatile size_t tail_;
};

} // namespace hackos::radio
