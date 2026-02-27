/**
 * @file desktop_view.cpp
 * @brief DesktopView implementation – HackOS home screen.
 *
 * Draws a top status bar (SD, WiFi, battery) and the HackOS logo /
 * brand text centred in the remaining area.
 */

#include "ui/desktop_view.h"

#include <cstdio>

#include "ui/canvas.h"

namespace
{
constexpr size_t STATUS_BUF_SIZE = 32U;
} // namespace

DesktopView::DesktopView()
    : batteryPercent_(100U),
      wifiConnected_(false),
      sdMounted_(false)
{
}

void DesktopView::setBatteryLevel(uint8_t percent)
{
    batteryPercent_ = percent > 100U ? 100U : percent;
}

void DesktopView::setWifiStatus(bool connected)
{
    wifiConnected_ = connected;
}

void DesktopView::setSdStatus(bool mounted)
{
    sdMounted_ = mounted;
}

// ── View interface ───────────────────────────────────────────────────────────

void DesktopView::draw(Canvas *canvas)
{
    drawStatusBar(canvas);
    drawMainArea(canvas);
}

bool DesktopView::input(InputEvent * /*event*/)
{
    // Desktop does not consume input events; let them propagate.
    return false;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void DesktopView::drawStatusBar(Canvas *canvas)
{
    // --- Status text: SD | WiFi ------------------------------------------------
    char statusText[STATUS_BUF_SIZE];
    std::snprintf(statusText, sizeof(statusText), "%s  %s",
                  sdMounted_ ? "SD" : "--",
                  wifiConnected_ ? "WiFi" : "----");
    canvas->drawStr(1, 1, statusText);

    // --- Battery icon (right-aligned) -----------------------------------------
    // Outer rectangle: 20×8 at the top-right corner.
    constexpr int16_t batX = Canvas::WIDTH - 22;
    constexpr int16_t batY = 0;
    constexpr int16_t batW = 20;
    constexpr int16_t batH = 8;
    canvas->drawRect(batX, batY, batW, batH);
    // Positive terminal nub.
    canvas->fillRect(static_cast<int16_t>(batX + batW), static_cast<int16_t>(batY + 2), 2, 4);

    // Fill proportional to percentage.
    const int16_t fillW = static_cast<int16_t>((static_cast<uint16_t>(batteryPercent_) * static_cast<uint16_t>(batW - 2U)) / 100U);
    if (fillW > 0)
    {
        canvas->fillRect(static_cast<int16_t>(batX + 1), static_cast<int16_t>(batY + 1), fillW, static_cast<int16_t>(batH - 2));
    }

    // --- Separator line -------------------------------------------------------
    canvas->drawLine(0, STATUS_BAR_HEIGHT, static_cast<int16_t>(Canvas::WIDTH - 1), STATUS_BAR_HEIGHT);
}

void DesktopView::drawMainArea(Canvas *canvas)
{
    // Centre the brand text vertically in the area below the status bar.
    constexpr int16_t areaTop = 12;  // below status bar + separator
    constexpr int16_t areaHeight = Canvas::HEIGHT - areaTop;

    // "HackOS" in the built-in 5×7 font (6 chars × 6 px = 36 px wide).
    constexpr int16_t titleW = 6 * (Canvas::FONT_WIDTH + Canvas::CHAR_SPACING);
    constexpr int16_t titleX = (Canvas::WIDTH - titleW) / 2;
    constexpr int16_t titleY = areaTop + (areaHeight / 2) - Canvas::FONT_HEIGHT - 2;

    canvas->drawStr(titleX, titleY, "HackOS");

    // Decorative frame around brand.
    constexpr int16_t frameMargin = 4;
    canvas->drawRect(
        static_cast<int16_t>(titleX - frameMargin),
        static_cast<int16_t>(titleY - frameMargin),
        static_cast<int16_t>(titleW + frameMargin * 2),
        static_cast<int16_t>(Canvas::FONT_HEIGHT + frameMargin * 2));

    // Tagline below the logo.
    constexpr int16_t tagW = 14 * (Canvas::FONT_WIDTH + Canvas::CHAR_SPACING);
    constexpr int16_t tagX = (Canvas::WIDTH - tagW) / 2;
    constexpr int16_t tagY = titleY + Canvas::FONT_HEIGHT + frameMargin + 4;
    canvas->drawStr(tagX, tagY, "Security Toolkit");
}
