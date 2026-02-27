/**
 * @file message_bus.h
 * @brief Global IPC message bus for HackOS FreeRTOS architecture.
 *
 * Provides a thread-safe message bus built on top of FreeRTOS
 * xMessageBuffer.  All inter-task communication MUST go through this
 * bus – tasks never call each other's functions directly.
 *
 * @note All public types live in the hackos::core namespace.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>
#include <freertos/semphr.h>

namespace hackos::core {

// ── Event descriptor ─────────────────────────────────────────────────────────

/// @brief Event types that flow through the message bus.
enum class HackOSEventType : uint8_t
{
    EVT_NONE = 0,
    EVT_INPUT,
    EVT_SYSTEM,
    EVT_APP,
    EVT_WIFI,
    EVT_RADIO,
    EVT_NFC,
    EVT_IR,
    EVT_POWER,
    EVT_STORAGE,
    EVT_CREDENTIALS, ///< Captured credentials from Captive Portal
};

/// @brief Fixed-size event structure published to the message bus.
struct HackOSEvent
{
    uint8_t type;     ///< HackOSEventType cast to uint8_t
    int32_t arg0;     ///< Primary argument (event-specific)
    int32_t arg1;     ///< Secondary argument (event-specific)
    void *payload;    ///< Optional heap-allocated payload (receiver owns it)
};

// ── MessageBus ───────────────────────────────────────────────────────────────

/**
 * @brief Singleton message bus backed by a FreeRTOS MessageBuffer.
 *
 * Design constraints:
 *  - The internal buffer is statically allocated to avoid heap
 *    fragmentation.
 *  - A mutex serialises concurrent writes so that multiple producer
 *    tasks can safely publish without corruption.
 *  - Consumers call receive() from their own task context; the call
 *    blocks for up to @c ticksToWait.
 */
class MessageBus
{
public:
    static MessageBus &instance();

    /**
     * @brief Initialise the message buffer and write-guard mutex.
     * @return true on success.
     */
    bool init();

    /**
     * @brief Publish an event (ISR-unsafe – use from task context only).
     * @param event  Event to send.
     * @param ticksToWait  Max ticks to wait for space in the buffer.
     * @return true if the event was written successfully.
     */
    bool publish(const HackOSEvent &event, TickType_t ticksToWait = 0);

    /**
     * @brief Blocking receive – pulls the next event from the buffer.
     * @param[out] event  Destination for the received event.
     * @param ticksToWait  Max ticks to block.
     * @return true if an event was received.
     */
    bool receive(HackOSEvent &event, TickType_t ticksToWait = portMAX_DELAY);

private:
    /// Buffer size in bytes – fits ~64 events at sizeof(HackOSEvent)==16.
    static constexpr size_t BUFFER_SIZE = 1024U;

    MessageBus();

    /// Static backing store for the message buffer (avoids heap alloc).
    /// The extra byte is required by xMessageBufferCreateStatic, which
    /// uses one byte internally to track write positions.
    uint8_t storage_[BUFFER_SIZE + 1U];
    StaticMessageBuffer_t bufferStruct_;
    MessageBufferHandle_t handle_;

    /// Mutex that serialises concurrent publish() calls.
    SemaphoreHandle_t writeMutex_;
    StaticSemaphore_t writeMutexBuf_;
};

} // namespace hackos::core
