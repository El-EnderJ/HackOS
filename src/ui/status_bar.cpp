/**
 * @file status_bar.cpp
 * @brief Dynamic status bar implementation.
 */

#include "ui/status_bar.h"

#include <cstdio>
#include <cstring>

namespace hackos {
namespace ui {

static constexpr uint16_t BLINK_PERIOD_MS   = 250U;   // Toggle every 250 ms.
static constexpr uint16_t ACTIVITY_TIMEOUT  = 1000U;  // SD icon disappears 1 s after last write.

StatusBar::StatusBar()
    : sdBlink_(false)
    , sdBlinkTimer_(0U)
    , sdActivityTimer_(ACTIVITY_TIMEOUT)
    , tx_(false)
    , hours_(0U)
    , minutes_(0U)
    , clockSet_(false)
    , uptimeMs_(0U)
{
}

void StatusBar::notifySdWrite()
{
    sdActivityTimer_ = 0U;
}

void StatusBar::setTransmitting(bool tx)
{
    tx_ = tx;
}

void StatusBar::setTime(uint8_t hours, uint8_t minutes)
{
    hours_    = hours;
    minutes_  = minutes;
    clockSet_ = true;
}

void StatusBar::tick(uint16_t frameDeltaMs)
{
    // SD blink timer.
    if (sdActivityTimer_ < ACTIVITY_TIMEOUT)
    {
        sdActivityTimer_ += frameDeltaMs;
        sdBlinkTimer_ += frameDeltaMs;
        if (sdBlinkTimer_ >= BLINK_PERIOD_MS)
        {
            sdBlinkTimer_ = 0U;
            sdBlink_ = !sdBlink_;
        }
    }
    else
    {
        sdBlink_ = false;
    }

    // Uptime counter for fallback clock.
    if (!clockSet_)
    {
        uptimeMs_ += frameDeltaMs;
    }
}

bool StatusBar::sdActive() const
{
    return (sdActivityTimer_ < ACTIVITY_TIMEOUT) && sdBlink_;
}

bool StatusBar::transmitting() const
{
    return tx_;
}

void StatusBar::clockString(char *buf, size_t bufLen) const
{
    if (buf == nullptr || bufLen < 6U)
    {
        return;
    }

    if (clockSet_)
    {
        snprintf(buf, bufLen, "%02u:%02u",
                 static_cast<unsigned>(hours_),
                 static_cast<unsigned>(minutes_));
    }
    else
    {
        // Fallback: show uptime as MM:SS.
        const uint32_t totalSec = uptimeMs_ / 1000U;
        const uint32_t mm = (totalSec / 60U) % 100U;
        const uint32_t ss = totalSec % 60U;
        snprintf(buf, bufLen, "%02lu:%02lu",
                 static_cast<unsigned long>(mm),
                 static_cast<unsigned long>(ss));
    }
}

} // namespace ui
} // namespace hackos
