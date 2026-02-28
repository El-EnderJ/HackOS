/**
 * @file hackbot_widget.h
 * @brief ASCII mascot widget – the Hack-Bot that evolves with user level.
 *
 * Displays a small ASCII-art character on the OLED whose appearance and
 * mood change as the player levels up.  Also renders a brief "Level Up!"
 * notification overlay when triggered.
 */

#pragma once

#include <cstdint>

#include "ui/widget.h"

class HackBotWidget : public Widget
{
public:
    /// @param x,y       Top-left corner of the widget area.
    /// @param width,h   Widget dimensions (e.g. 40×24).
    HackBotWidget(int16_t x, int16_t y, int16_t width, int16_t height);

    /// @brief Update the displayed level (changes mascot art).
    void setLevel(uint16_t level);

    /// @brief Set current XP progress bar value (0–100 %).
    void setXPProgress(uint8_t percent);

    /// @brief Flash "Level Up!" overlay for a few draw cycles.
    void showLevelUp();

    void draw() override;

private:
    /// @brief Return the ASCII art lines for the current level tier.
    const char *const *mascotArt() const;

    uint16_t level_;
    uint8_t xpPercent_;
    uint8_t levelUpFrames_; ///< countdown of frames to show the overlay
};
