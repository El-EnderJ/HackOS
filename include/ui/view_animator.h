/**
 * @file view_animator.h
 * @brief Transition engine for smooth horizontal slide animations.
 *
 * ViewAnimator manages animated transitions between scenes in the
 * SceneManager.  It uses double buffering to prevent flicker: one
 * buffer holds the outgoing scene and the other the incoming scene.
 * During an animation tick the two buffers are alpha-blended (pixel
 * shift) onto the output canvas.
 *
 * Usage:
 * @code
 *   hackos::ui::ViewAnimator anim;
 *   anim.startSlide(ViewAnimator::Direction::LEFT, 200);
 *   while (anim.isAnimating()) {
 *       anim.tick(canvas);
 *   }
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace hackos {
namespace ui {

class ViewAnimator
{
public:
    /// Slide direction for scene transitions.
    enum class Direction : uint8_t
    {
        LEFT,   ///< New scene slides in from the right.
        RIGHT,  ///< New scene slides in from the left.
    };

    static constexpr int16_t SCREEN_W = 128;
    static constexpr int16_t SCREEN_H = 64;
    static constexpr size_t  BUF_SIZE = static_cast<size_t>(SCREEN_W) * static_cast<size_t>(SCREEN_H) / 8U;

    ViewAnimator();

    /**
     * @brief Capture the current framebuffer as the "outgoing" scene.
     * @param src  Pointer to a 1024-byte SSD1306-format buffer.
     */
    void captureOutgoing(const uint8_t *src);

    /**
     * @brief Capture the new framebuffer as the "incoming" scene.
     * @param src  Pointer to a 1024-byte SSD1306-format buffer.
     */
    void captureIncoming(const uint8_t *src);

    /**
     * @brief Begin a horizontal slide animation.
     * @param dir        Slide direction.
     * @param durationMs Total animation time in milliseconds.
     */
    void startSlide(Direction dir, uint16_t durationMs = 200U);

    /**
     * @brief Advance the animation by one frame.
     *
     * Composites the two captured buffers into @p dst according to
     * the current progress.  Call this once per render frame (~30 FPS).
     *
     * @param dst        Output buffer to composite into (1024 bytes).
     * @param frameDelta Milliseconds elapsed since the last tick.
     * @return true while the animation is still in progress.
     */
    bool tick(uint8_t *dst, uint16_t frameDelta = 33U);

    /// @return true if a transition is currently running.
    bool isAnimating() const;

    /// @brief Cancel any running animation.
    void cancel();

private:
    /// Read a single pixel from an SSD1306 page-addressed buffer.
    static bool readPixel(const uint8_t *buf, int16_t x, int16_t y);

    /// Write a single pixel into an SSD1306 page-addressed buffer.
    static void writePixel(uint8_t *buf, int16_t x, int16_t y, bool on);

    uint8_t  bufOut_[BUF_SIZE];   ///< Outgoing scene snapshot.
    uint8_t  bufIn_[BUF_SIZE];    ///< Incoming scene snapshot.
    Direction dir_;
    uint16_t durationMs_;
    uint16_t elapsedMs_;
    bool     active_;
};

} // namespace ui
} // namespace hackos
