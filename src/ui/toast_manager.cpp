/**
 * @file toast_manager.cpp
 * @brief Global toast notification overlay implementation.
 */

#include "ui/toast_manager.h"

#include <Arduino.h>
#include <cstring>
#include <esp_log.h>

#include "config.h"
#include "hardware/display.h"

static constexpr const char *TAG_TOAST = "Toast";

// ── Singleton ────────────────────────────────────────────────────────────────

ToastManager &ToastManager::instance()
{
    static ToastManager mgr;
    return mgr;
}

ToastManager::ToastManager()
    : message_{0},
      showUntilMs_(0U),
      active_(false)
{
}

// ── Public API ──────────────────────────────────────────────────────────────

void ToastManager::show(const char *message, uint32_t durationMs)
{
    if (message == nullptr || message[0] == '\0')
    {
        return;
    }

    std::strncpy(message_, message, MAX_MSG_LEN);
    message_[MAX_MSG_LEN] = '\0';
    showUntilMs_ = millis() + durationMs;
    active_ = true;
    hapticBuzz();
    ESP_LOGI(TAG_TOAST, "Toast: %s (%lu ms)", message_, static_cast<unsigned long>(durationMs));
}

void ToastManager::draw()
{
    if (!active_)
    {
        return;
    }

    if (millis() >= showUntilMs_)
    {
        active_ = false;
        return;
    }

    auto &disp = DisplayManager::instance();
    // Draw a filled black rectangle as background with a white border.
    disp.fillRect(TOAST_X, TOAST_Y, TOAST_W, TOAST_H, SSD1306_BLACK);
    disp.drawRect(TOAST_X, TOAST_Y, TOAST_W, TOAST_H, SSD1306_WHITE);
    disp.drawText(TOAST_X + TEXT_PAD_X, TOAST_Y + TEXT_PAD_Y, message_, 1U, SSD1306_WHITE);
}

bool ToastManager::isActive() const
{
    return active_;
}

void ToastManager::onEvent(Event *event)
{
    if (event == nullptr || event->type != EventType::EVT_TOAST)
    {
        return;
    }

    const uint32_t dur = (event->arg0 > 0) ? static_cast<uint32_t>(event->arg0) : 3000U;
    const char *msg = static_cast<const char *>(event->data);
    if (msg != nullptr)
    {
        show(msg, dur);
    }
}

// ── Private ─────────────────────────────────────────────────────────────────

void ToastManager::hapticBuzz()
{
    ledcAttachPin(PIN_BUZZER, 0);
    ledcWriteTone(0, BUZZ_FREQ);
    delay(BUZZ_MS);
    ledcWriteTone(0, 0);
    ledcDetachPin(PIN_BUZZER);
}
