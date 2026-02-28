/**
 * @file toast_manager.h
 * @brief Global toast notification overlay ("Overwatch" system).
 *
 * Any background task can publish a toast via the EventSystem or call
 * ToastManager::show() directly.  The active toast is rendered as a
 * small pop-up at the bottom of the OLED without interrupting the
 * running app.  A short haptic buzz on the buzzer accompanies each
 * new toast.
 *
 * Usage from any task:
 * @code
 *   ToastManager::instance().show("[!] Handshake: MiWiFi");
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/event_system.h"

class ToastManager : public IEventObserver
{
public:
    static ToastManager &instance();

    /// @brief Display a toast message for @p durationMs milliseconds.
    void show(const char *message, uint32_t durationMs = 3000U);

    /// @brief Called every frame from the main loop – draws the overlay
    ///        if a toast is active and dismisses it when expired.
    void draw();

    /// @brief Returns true if a toast is currently visible.
    bool isActive() const;

    /// @brief Handle EVT_TOAST events from the EventSystem.
    void onEvent(Event *event) override;

private:
    static constexpr size_t MAX_MSG_LEN = 40U;

    /// Toast pop-up geometry (bottom of 128×64 OLED).
    static constexpr int16_t TOAST_Y      = 50;
    static constexpr int16_t TOAST_H      = 14;
    static constexpr int16_t TOAST_X      = 0;
    static constexpr int16_t TOAST_W      = 128;
    static constexpr int16_t TEXT_PAD_X   = 3;
    static constexpr int16_t TEXT_PAD_Y   = 3;

    /// Haptic buzz duration in milliseconds.
    static constexpr uint32_t BUZZ_MS     = 80U;
    /// Buzzer PWM frequency (Hz).
    static constexpr uint32_t BUZZ_FREQ   = 2000U;

    ToastManager();

    void hapticBuzz();

    char message_[MAX_MSG_LEN + 1U];
    uint32_t showUntilMs_;
    bool active_;
};
