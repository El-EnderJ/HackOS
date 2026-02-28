/**
 * @file status_bar.h
 * @brief Dynamic status bar for the HackOS GUI task.
 *
 * Renders a top-row bar showing:
 *  - SD activity indicator (blinks while writing).
 *  - TX indicator (upward arrow when RF/IR is transmitting).
 *  - System clock (NTP/GPS-synchronised or uptime fallback).
 *
 * The bar height is 10 pixels (matching the header separator in apps).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos {
namespace ui {

class StatusBar
{
public:
    static constexpr int16_t BAR_HEIGHT = 10;
    static constexpr int16_t BAR_WIDTH  = 128;

    StatusBar();

    /// @brief Notify that the SD card is being written.  Resets automatically.
    void notifySdWrite();

    /// @brief Set whether the device is currently transmitting (RF/IR).
    void setTransmitting(bool tx);

    /// @brief Set the system clock (hh:mm).  Pass nullptr to use uptime.
    void setTime(uint8_t hours, uint8_t minutes);

    /// @brief Call once per frame to advance blink timers.
    void tick(uint16_t frameDeltaMs = 33U);

    // ── Query for rendering ──────────────────────────────────────────────

    bool sdActive() const;     ///< true when SD icon should be visible.
    bool transmitting() const; ///< true when TX arrow should be shown.

    /// @brief Write a "HH:MM" string into @p buf (at least 6 chars).
    void clockString(char *buf, size_t bufLen) const;

private:
    bool     sdBlink_;           ///< Current blink phase.
    uint16_t sdBlinkTimer_;      ///< Countdown for blink toggle.
    uint16_t sdActivityTimer_;   ///< Frames since last SD write notification.
    bool     tx_;

    uint8_t  hours_;
    uint8_t  minutes_;
    bool     clockSet_;          ///< true when NTP/GPS time has been received.
    uint32_t uptimeMs_;          ///< Fallback uptime counter.
};

} // namespace ui
} // namespace hackos
