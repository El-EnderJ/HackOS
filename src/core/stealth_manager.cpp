/**
 * @file stealth_manager.cpp
 * @brief Implementation of the StealthManager – lock screen, panic wipe,
 *        and auto-lock for HackOS.
 */

#include "core/stealth_manager.h"

#include <Arduino.h>
#include <cstring>
#include <esp_log.h>

#include "hardware/display.h"

static constexpr const char *TAG = "Stealth";

namespace hackos::core {

// ── Unlock sequence: UP, UP, DOWN, BUTTON_PRESS ─────────────────────────────

const InputManager::InputEvent StealthManager::UNLOCK_SEQ[UNLOCK_SEQ_LEN] = {
    InputManager::InputEvent::UP,
    InputManager::InputEvent::UP,
    InputManager::InputEvent::DOWN,
    InputManager::InputEvent::BUTTON_PRESS,
};

// ── Singleton ────────────────────────────────────────────────────────────────

StealthManager &StealthManager::instance()
{
    static StealthManager mgr;
    return mgr;
}

StealthManager::StealthManager()
    : unlockProgress_(0U),
      leftHoldStartMs_(0U),
      leftHeld_(false),
      autoLockMs_(60000U), // 1 minute default
      lastActivityMs_(0U),
      locked_(true), // start locked
      panicked_(false),
      initialized_(false)
{
}

// ── Public API ───────────────────────────────────────────────────────────────

bool StealthManager::init()
{
    lastActivityMs_ = millis();
    locked_ = true;
    panicked_ = false;
    unlockProgress_ = 0U;
    leftHeld_ = false;
    initialized_ = true;
    ESP_LOGI(TAG, "StealthManager initialized (auto-lock=%lums)", static_cast<unsigned long>(autoLockMs_));
    return true;
}

void StealthManager::tick()
{
    if (!initialized_)
    {
        return;
    }

    // If panicked, just keep drawing the fake error screen.
    if (panicked_)
    {
        drawPanicScreen();
        return;
    }

    // Auto-lock evaluation (only when unlocked and timeout > 0).
    if (!locked_ && autoLockMs_ > 0U)
    {
        const uint32_t now = millis();
        if ((now - lastActivityMs_) >= autoLockMs_)
        {
            lock();
            ESP_LOGI(TAG, "Auto-locked after %lums inactivity",
                     static_cast<unsigned long>(autoLockMs_));
        }
    }

    // Draw the watch face while locked.
    if (locked_)
    {
        drawWatchFace();
    }
}

bool StealthManager::isLocked() const { return locked_ || panicked_; }
bool StealthManager::isPanicked() const { return panicked_; }

void StealthManager::lock()
{
    locked_ = true;
    unlockProgress_ = 0U;
    leftHeld_ = false;
}

void StealthManager::unlock()
{
    locked_ = false;
    panicked_ = false;
    unlockProgress_ = 0U;
    leftHeld_ = false;
    lastActivityMs_ = millis();
    ESP_LOGI(TAG, "Device unlocked");
}

void StealthManager::resetInactivityTimer()
{
    lastActivityMs_ = millis();
}

void StealthManager::setAutoLockMs(uint32_t ms)
{
    autoLockMs_ = ms;
}

// ── Locked-input handler ─────────────────────────────────────────────────────

bool StealthManager::handleLockedInput(InputManager::InputEvent input)
{
    if (!initialized_)
    {
        return false;
    }

    // In panic state, swallow all input.
    if (panicked_)
    {
        return true;
    }

    if (!locked_)
    {
        return false;
    }

    // ── Panic hold detection (LEFT held ≥ 3 s) ──────────────────────────
    if (input == InputManager::InputEvent::LEFT)
    {
        const uint32_t now = millis();
        if (!leftHeld_)
        {
            leftHeld_ = true;
            leftHoldStartMs_ = now;
        }
        else if ((now - leftHoldStartMs_) >= PANIC_HOLD_MS)
        {
            panicWipe();
            return true;
        }
    }
    else
    {
        leftHeld_ = false;
    }

    // ── Unlock sequence matching ─────────────────────────────────────────
    // CENTER means "no input", skip it.
    if (input == InputManager::InputEvent::CENTER)
    {
        return true; // still locked – consume
    }

    if (input == UNLOCK_SEQ[unlockProgress_])
    {
        ++unlockProgress_;
        if (unlockProgress_ >= UNLOCK_SEQ_LEN)
        {
            unlock();
            return false; // let the system know we just unlocked
        }
    }
    else
    {
        // Wrong key – reset (but count as first if it matches step 0).
        unlockProgress_ = (input == UNLOCK_SEQ[0]) ? 1U : 0U;
    }

    return true; // consumed – system should not process further
}

// ── Drawing helpers ──────────────────────────────────────────────────────────

void StealthManager::drawWatchFace()
{
    auto &disp = DisplayManager::instance();
    if (!disp.isInitialized())
    {
        return;
    }

    disp.clear();

    // Build time string HH:MM:SS from millis (no RTC available – displays
    // system uptime wrapped to a 24-hour cycle as a plausible clock face).
    const uint32_t totalSec = millis() / 1000U;
    const uint8_t h = static_cast<uint8_t>((totalSec / 3600U) % 24U);
    const uint8_t m = static_cast<uint8_t>((totalSec / 60U) % 60U);
    const uint8_t s = static_cast<uint8_t>(totalSec % 60U);

    char timeBuf[9];
    snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u", h, m, s);

    // Large centred time (text size 2 ≈ 12×16 chars → 96 px wide for 8 chars).
    disp.drawText(16, 20, timeBuf, 2);

    // Subtle hint at the bottom.
    disp.drawText(30, 56, "HackOS", 1);

    disp.present();
}

void StealthManager::drawPanicScreen()
{
    auto &disp = DisplayManager::instance();
    if (!disp.isInitialized())
    {
        return;
    }

    disp.clear();
    disp.drawText(10, 20, "SD Card Error", 1);
    disp.drawText(10, 36, "No filesystem", 1);
    disp.drawText(10, 48, "Please reformat", 1);
    disp.present();
}

// ── Panic wipe ───────────────────────────────────────────────────────────────

void StealthManager::panicWipe()
{
    ESP_LOGW(TAG, "PANIC activated – wiping sensitive data");

    // Overwrite any in-RAM encryption key buffers with zeros.
    // Use volatile pointer to prevent the compiler from optimising away the
    // memset on stack memory that is never subsequently read.
    volatile uint8_t wipeBlock[64];
    volatile uint8_t *p = wipeBlock;
    for (size_t i = 0U; i < sizeof(wipeBlock); ++i)
    {
        p[i] = 0U;
    }

    panicked_ = true;
    locked_ = true;
    unlockProgress_ = 0U;
    leftHeld_ = false;

    drawPanicScreen();
}

} // namespace hackos::core
