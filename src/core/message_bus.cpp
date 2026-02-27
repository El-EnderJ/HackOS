/**
 * @file message_bus.cpp
 * @brief MessageBus implementation – global IPC via FreeRTOS MessageBuffer.
 */

#include "core/message_bus.h"

#include <cstring>
#include <esp_log.h>

static constexpr const char *TAG = "MessageBus";

namespace hackos::core {

// ── Singleton ────────────────────────────────────────────────────────────────

MessageBus &MessageBus::instance()
{
    static MessageBus bus;
    return bus;
}

MessageBus::MessageBus()
    : storage_{},
      bufferStruct_{},
      handle_(nullptr),
      writeMutex_(nullptr),
      writeMutexBuf_{}
{
}

// ── Public API ───────────────────────────────────────────────────────────────

bool MessageBus::init()
{
    if (handle_ != nullptr)
    {
        return true; // already initialised
    }

    handle_ = xMessageBufferCreateStatic(BUFFER_SIZE, storage_, &bufferStruct_);
    if (handle_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create message buffer");
        return false;
    }

    writeMutex_ = xSemaphoreCreateMutexStatic(&writeMutexBuf_);
    if (writeMutex_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create write mutex");
        return false;
    }

    ESP_LOGI(TAG, "Initialised (buffer %u bytes)", static_cast<unsigned>(BUFFER_SIZE));
    return true;
}

bool MessageBus::publish(const HackOSEvent &event, TickType_t ticksToWait)
{
    if (handle_ == nullptr || writeMutex_ == nullptr)
    {
        return false;
    }

    if (xSemaphoreTake(writeMutex_, ticksToWait) != pdTRUE)
    {
        ESP_LOGD(TAG, "publish: mutex timeout");
        return false;
    }

    const size_t sent = xMessageBufferSend(handle_, &event, sizeof(HackOSEvent), ticksToWait);
    xSemaphoreGive(writeMutex_);

    if (sent == 0U)
    {
        ESP_LOGD(TAG, "publish: buffer full, event type=%u dropped", event.type);
        return false;
    }

    return true;
}

bool MessageBus::receive(HackOSEvent &event, TickType_t ticksToWait)
{
    if (handle_ == nullptr)
    {
        return false;
    }

    const size_t received = xMessageBufferReceive(handle_, &event, sizeof(HackOSEvent), ticksToWait);
    return received == sizeof(HackOSEvent);
}

} // namespace hackos::core
