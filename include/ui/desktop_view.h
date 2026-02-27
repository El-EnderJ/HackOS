/**
 * @file desktop_view.h
 * @brief HackOS desktop view – the "home screen".
 *
 * Displays a top status bar (battery percentage, Wi-Fi indicator,
 * SD-card indicator) and a centred HackOS logo / brand text in
 * the main area.
 */

#pragma once

#include <cstdint>

#include "ui/view.h"

class Canvas;

class DesktopView : public View
{
public:
    DesktopView();

    // ── Status setters ───────────────────────────────────────────────────

    void setBatteryLevel(uint8_t percent);
    void setWifiStatus(bool connected);
    void setSdStatus(bool mounted);

    // ── View interface ───────────────────────────────────────────────────

    void draw(Canvas *canvas) override;
    bool input(InputEvent *event) override;

private:
    /// Height reserved for the status bar (pixels).
    static constexpr int16_t STATUS_BAR_HEIGHT = 10;

    void drawStatusBar(Canvas *canvas);
    void drawMainArea(Canvas *canvas);

    uint8_t batteryPercent_;
    bool wifiConnected_;
    bool sdMounted_;
};
