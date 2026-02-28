#include "ui/hackbot_widget.h"

#include <cstdio>

#include "hardware/display.h"

// ── Mascot ASCII art tiers (3 lines each, max ~7 chars wide) ─────────────────
// Tier 0: Newbie   (level 1–4)
// Tier 1: Hacker   (level 5–9)
// Tier 2: Elite    (level 10–19)
// Tier 3: Legend   (level 20+)

static const char *const TIER0_ART[] = {" [o_o]", "  /|\\", "  / \\"};
static const char *const TIER1_ART[] = {" [>_<]", " </|\\>", "  / \\"};
static const char *const TIER2_ART[] = {" {O_O}", " <[|]>", "  /|\\"};
static const char *const TIER3_ART[] = {" <*_*>", " /{|}\\", " _/ \\_"};

// ── Mood strings ─────────────────────────────────────────────────────────────

static const char *moodForLevel(uint16_t level)
{
    if (level >= 20U) return "Legendary!";
    if (level >= 10U) return "Elite";
    if (level >= 5U)  return "Skilled";
    return "Newbie";
}

// ── HackBotWidget ────────────────────────────────────────────────────────────

HackBotWidget::HackBotWidget(int16_t x, int16_t y, int16_t w, int16_t h)
    : Widget(x, y, w, h),
      level_(1U),
      xpPercent_(0U),
      levelUpFrames_(0U)
{
}

void HackBotWidget::setLevel(uint16_t level)
{
    if (level != level_)
    {
        level_ = level;
        markDirty();
    }
}

void HackBotWidget::setXPProgress(uint8_t percent)
{
    if (percent != xpPercent_)
    {
        xpPercent_ = percent;
        markDirty();
    }
}

void HackBotWidget::showLevelUp()
{
    levelUpFrames_ = 30U; // show overlay for ~30 draw cycles
    markDirty();
}

const char *const *HackBotWidget::mascotArt() const
{
    if (level_ >= 20U) return TIER3_ART;
    if (level_ >= 10U) return TIER2_ART;
    if (level_ >= 5U)  return TIER1_ART;
    return TIER0_ART;
}

void HackBotWidget::draw()
{
    auto &disp = DisplayManager::instance();

    // Draw mascot (3 lines of ASCII)
    const char *const *art = mascotArt();
    for (int i = 0; i < 3; ++i)
    {
        disp.drawText(x_, static_cast<int16_t>(y_ + i * 9), art[i], 1U);
    }

    // Level label
    char label[24];
    std::snprintf(label, sizeof(label), "Lv%u %s", static_cast<unsigned>(level_), moodForLevel(level_));
    disp.drawText(x_, static_cast<int16_t>(y_ + 28), label, 1U);

    // Mini XP bar (width proportional to xpPercent_)
    const int16_t barW = static_cast<int16_t>((width_ * xpPercent_) / 100U);
    disp.drawRect(x_, static_cast<int16_t>(y_ + 38), width_, 4);
    if (barW > 0)
    {
        disp.fillRect(x_, static_cast<int16_t>(y_ + 38), barW, 4);
    }

    // Level-up notification overlay
    if (levelUpFrames_ > 0U)
    {
        disp.fillRect(14, 20, 100, 24, 0U); // black background
        disp.drawRect(14, 20, 100, 24);
        disp.drawText(24, 26, "LEVEL UP!", 1U);
        char lvlBuf[16];
        std::snprintf(lvlBuf, sizeof(lvlBuf), "-> Lv%u <-", static_cast<unsigned>(level_));
        disp.drawText(30, 36, lvlBuf, 1U);
        --levelUpFrames_;
        if (levelUpFrames_ > 0U)
        {
            markDirty(); // keep redrawing until overlay expires
        }
    }
}
