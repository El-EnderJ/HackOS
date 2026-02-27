/**
 * @file power_manager.h
 * @brief CPU-frequency scaling and light-sleep controller for HackOS.
 *
 * The PowerManager reduces power consumption by:
 *  1. Scaling the CPU down from 240 MHz to 80 MHz when the Launcher
 *     is idle (no user input for a configurable timeout).
 *  2. Entering ESP32 Light Sleep when idle for an even longer period,
 *     waking on a joystick button press via GPIO interrupt.
 *
 * @note Lives in the hackos::core namespace.
 */

#pragma once

#include <cstdint>

namespace hackos::core {

/**
 * @brief Singleton that owns CPU-frequency and sleep policy.
 *
 * Thread safety: all public methods are safe to call from any task
 * (state is guarded by an internal critical section where needed).
 */
class PowerManager
{
public:
    static PowerManager &instance();

    /**
     * @brief Initialise GPIO wake-up source and set initial frequency.
     * @param wakeGpio  GPIO number for joystick button (active-low).
     * @return true on success.
     */
    bool init(uint8_t wakeGpio);

    /// @brief Set CPU to high-performance mode (240 MHz).
    void setHighPerformance();

    /// @brief Set CPU to low-power mode (80 MHz).
    void setLowPower();

    /// @brief Return the current CPU frequency in MHz.
    uint32_t currentFrequencyMHz() const;

    /**
     * @brief Notify the manager that user activity occurred.
     *
     * Resets the idle timer.  If the CPU was scaled down, it is
     * immediately boosted back to 240 MHz.
     */
    void onActivity();

    /**
     * @brief Call periodically (e.g. from Core_Task) to evaluate
     *        whether the CPU should be scaled down or the chip
     *        should enter Light Sleep.
     *
     * @param idleThresholdMs  Time without activity before scaling
     *        down the CPU (default 30 s).
     * @param sleepThresholdMs Time without activity before entering
     *        Light Sleep (default 60 s).
     */
    void tick(uint32_t idleThresholdMs = 30000U,
              uint32_t sleepThresholdMs = 60000U);

    /**
     * @brief Enter ESP32 Light Sleep immediately.
     *
     * The chip wakes on the joystick GPIO configured in init().
     */
    void enterLightSleep();

private:
    PowerManager();

    uint8_t wakeGpio_;
    uint32_t lastActivityMs_;
    bool lowPowerActive_;
    bool initialized_;
};

} // namespace hackos::core
