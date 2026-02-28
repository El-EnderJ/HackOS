/**
 * @file stealth_manager.h
 * @brief Stealth & security manager for HackOS – lock screen, panic wipe,
 *        and auto-lock.
 *
 * Features:
 *  - **Watch Mode**: A digital-clock lock screen.  The operator must enter a
 *    secret joystick sequence (Up, Up, Down, Button) to unlock.
 *  - **Panic Sequence**: Holding LEFT for 3 seconds wipes the encryption key
 *    from RAM and displays a fake "SD Error" message.
 *  - **Auto-Lock**: Configurable inactivity timer (default 60 s) that
 *    automatically re-engages the lock screen.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "hardware/input.h"

namespace hackos::core {

class StealthManager
{
public:
    static StealthManager &instance();

    /**
     * @brief Initialise the stealth subsystem.
     * @return true on success.
     */
    bool init();

    /**
     * @brief Call every main-loop tick.
     *
     * Handles auto-lock timeout evaluation, unlock-sequence matching,
     * panic-hold detection, and lock-screen rendering.
     */
    void tick();

    // ── Query / control ──────────────────────────────────────────────────

    /// @brief Is the device currently locked (Watch Mode)?
    bool isLocked() const;

    /// @brief Is the device in panic state (fake SD Error)?
    bool isPanicked() const;

    /// @brief Lock the device immediately (enter Watch Mode).
    void lock();

    /// @brief Unlock the device (only used internally after correct sequence).
    void unlock();

    /// @brief Reset the inactivity timer (called on every user input).
    void resetInactivityTimer();

    /// @brief Set the auto-lock timeout in milliseconds (0 = disabled).
    void setAutoLockMs(uint32_t ms);

    /**
     * @brief Feed a joystick input event while locked.
     *
     * Returns true if the event was consumed by the stealth layer
     * (i.e. the rest of the system should ignore it).
     */
    bool handleLockedInput(InputManager::InputEvent input);

private:
    StealthManager();

    /// Draw the digital-clock lock screen on the OLED.
    void drawWatchFace();

    /// Draw the fake "SD Error" panic screen.
    void drawPanicScreen();

    /// Wipe sensitive data from RAM.
    void panicWipe();

    // ── Unlock sequence ──────────────────────────────────────────────────

    /// Expected unlock sequence: UP, UP, DOWN, BUTTON_PRESS
    static constexpr size_t UNLOCK_SEQ_LEN = 4U;
    static const InputManager::InputEvent UNLOCK_SEQ[UNLOCK_SEQ_LEN];

    size_t unlockProgress_;        ///< How many steps matched so far.

    // ── Panic detection ──────────────────────────────────────────────────

    static constexpr uint32_t PANIC_HOLD_MS = 3000U; ///< Hold LEFT for 3 s.
    uint32_t leftHoldStartMs_;     ///< millis() when LEFT was first seen.
    bool leftHeld_;                ///< Is LEFT currently being held?

    // ── Auto-lock ────────────────────────────────────────────────────────

    uint32_t autoLockMs_;          ///< Timeout in ms (0 = disabled).
    uint32_t lastActivityMs_;      ///< millis() of most recent user input.

    // ── State ────────────────────────────────────────────────────────────

    bool locked_;
    bool panicked_;
    bool initialized_;
};

} // namespace hackos::core
