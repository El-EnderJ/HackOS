/**
 * @file view_animator.cpp
 * @brief ViewAnimator – horizontal slide transition engine with double buffering.
 */

#include "ui/view_animator.h"

#include <cstring>
#include <algorithm>

namespace hackos {
namespace ui {

ViewAnimator::ViewAnimator()
    : dir_(Direction::LEFT)
    , durationMs_(200U)
    , elapsedMs_(0U)
    , active_(false)
{
    memset(bufOut_, 0, BUF_SIZE);
    memset(bufIn_,  0, BUF_SIZE);
}

void ViewAnimator::captureOutgoing(const uint8_t *src)
{
    if (src != nullptr)
    {
        memcpy(bufOut_, src, BUF_SIZE);
    }
}

void ViewAnimator::captureIncoming(const uint8_t *src)
{
    if (src != nullptr)
    {
        memcpy(bufIn_, src, BUF_SIZE);
    }
}

void ViewAnimator::startSlide(Direction dir, uint16_t durationMs)
{
    dir_        = dir;
    durationMs_ = (durationMs > 0U) ? durationMs : 1U;
    elapsedMs_  = 0U;
    active_     = true;
}

bool ViewAnimator::tick(uint8_t *dst, uint16_t frameDelta)
{
    if (!active_ || dst == nullptr)
    {
        return false;
    }

    elapsedMs_ += frameDelta;
    if (elapsedMs_ >= durationMs_)
    {
        // Animation complete – copy incoming buffer directly.
        memcpy(dst, bufIn_, BUF_SIZE);
        active_ = false;
        return false;
    }

    // Compute horizontal pixel offset (0 → SCREEN_W).
    const int16_t offset = static_cast<int16_t>(
        (static_cast<uint32_t>(elapsedMs_) * SCREEN_W) / durationMs_);

    // Clear output buffer.
    memset(dst, 0, BUF_SIZE);

    // Composite outgoing and incoming scenes side-by-side.
    for (int16_t y = 0; y < SCREEN_H; ++y)
    {
        for (int16_t x = 0; x < SCREEN_W; ++x)
        {
            int16_t srcOutX, srcInX;

            if (dir_ == Direction::LEFT)
            {
                // Outgoing slides left, incoming enters from right.
                srcOutX = x + offset;         // reads right-shifted pixel
                srcInX  = x - (SCREEN_W - offset);
            }
            else
            {
                // Outgoing slides right, incoming enters from left.
                srcOutX = x - offset;
                srcInX  = x + (SCREEN_W - offset);
            }

            bool pixel = false;

            if (srcOutX >= 0 && srcOutX < SCREEN_W)
            {
                pixel = readPixel(bufOut_, srcOutX, y);
            }
            else if (srcInX >= 0 && srcInX < SCREEN_W)
            {
                pixel = readPixel(bufIn_, srcInX, y);
            }

            if (pixel)
            {
                writePixel(dst, x, y, true);
            }
        }
    }

    return true; // Still animating.
}

bool ViewAnimator::isAnimating() const
{
    return active_;
}

void ViewAnimator::cancel()
{
    active_ = false;
}

// ── Pixel helpers (SSD1306 page-addressing) ──────────────────────────────────

bool ViewAnimator::readPixel(const uint8_t *buf, int16_t x, int16_t y)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
    {
        return false;
    }
    const size_t page = static_cast<size_t>(y) / 8U;
    const uint8_t bit = static_cast<uint8_t>(y) & 7U;
    return (buf[page * SCREEN_W + x] >> bit) & 1U;
}

void ViewAnimator::writePixel(uint8_t *buf, int16_t x, int16_t y, bool on)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
    {
        return;
    }
    const size_t page = static_cast<size_t>(y) / 8U;
    const uint8_t bit = static_cast<uint8_t>(y) & 7U;
    if (on)
    {
        buf[page * SCREEN_W + x] |= (1U << bit);
    }
    else
    {
        buf[page * SCREEN_W + x] &= ~(1U << bit);
    }
}

} // namespace ui
} // namespace hackos
