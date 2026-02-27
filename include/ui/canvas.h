/**
 * @file canvas.h
 * @brief Abstract canvas for the HackOS UI framework.
 *
 * Maintains a 1024-byte (128x64 / 8) monochrome frame buffer in RAM.
 * All drawing operations target this buffer; actual OLED transfer is
 * performed by the Gui render loop.
 *
 * Buffer layout follows SSD1306 page addressing: each byte stores
 * eight vertical pixels in a single column (bit 0 = topmost pixel
 * within that page).
 */

#pragma once

#include <cstddef>
#include <cstdint>

class Canvas
{
public:
    static constexpr int16_t WIDTH = 128;
    static constexpr int16_t HEIGHT = 64;
    static constexpr size_t BUFFER_SIZE = static_cast<size_t>(WIDTH) * static_cast<size_t>(HEIGHT) / 8U;

    /// Character cell dimensions for the built-in 5x7 monospace font.
    static constexpr uint8_t FONT_WIDTH = 5U;
    static constexpr uint8_t FONT_HEIGHT = 7U;
    static constexpr uint8_t CHAR_SPACING = 1U;

    Canvas();

    // ── Buffer access ────────────────────────────────────────────────────

    /// @brief Clear the frame buffer (all pixels off).
    void clear();

    /// @brief Raw read-only access to the frame buffer.
    const uint8_t *buffer() const;

    // ── Geometric primitives ─────────────────────────────────────────────

    void drawPixel(int16_t x, int16_t y, bool color = true);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool color = true);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool color = true);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, bool color = true);
    void drawCircle(int16_t cx, int16_t cy, int16_t r, bool color = true);
    void fillCircle(int16_t cx, int16_t cy, int16_t r, bool color = true);

    // ── Text (built-in 5×7 monospace font) ───────────────────────────────

    /// @brief Draw a single character at pixel position (x, y).
    void drawChar(int16_t x, int16_t y, char c, bool color = true);

    /// @brief Draw a NUL-terminated string at pixel position (x, y).
    void drawStr(int16_t x, int16_t y, const char *text, bool color = true);

    // ── Bitmap rendering ─────────────────────────────────────────────────

    /**
     * @brief Render an XBM bitmap.
     *
     * @param x      Top-left X position.
     * @param y      Top-left Y position.
     * @param w      Bitmap width in pixels.
     * @param h      Bitmap height in pixels.
     * @param bitmap Pointer to XBM data (LSB-first, row-major, rows padded
     *               to byte boundaries).
     */
    void drawXBM(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bitmap);

private:
    uint8_t buffer_[BUFFER_SIZE];
};
