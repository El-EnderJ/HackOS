/**
 * @file canvas.cpp
 * @brief Canvas implementation – software-rendered monochrome frame buffer.
 *
 * Drawing primitives operate directly on a 1024-byte RAM buffer laid out
 * in SSD1306 page-addressing order.  An embedded 5×7 monospace font
 * covers printable ASCII (0x20–0x7E).
 */

#include "ui/canvas.h"

#include <cstdlib>
#include <cstring>

// ── Embedded 5×7 monospace font ──────────────────────────────────────────────
// Each character is 5 bytes (one byte per column, LSB = top row).
// Characters 0x20 (' ') through 0x7E ('~') – 95 glyphs.

// NOLINTBEGIN(readability-magic-numbers)
static const uint8_t font5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, //   (space)
    0x00, 0x00, 0x5F, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7F, 0x14, 0x7F, 0x14, // #
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x00, 0x07, 0x00, 0x00, // '
    0x00, 0x1C, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1C, 0x00, // )
    0x14, 0x08, 0x3E, 0x08, 0x14, // *
    0x08, 0x08, 0x3E, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0
    0x00, 0x42, 0x7F, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4B, 0x31, // 3
    0x18, 0x14, 0x12, 0x7F, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1E, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x08, 0x14, 0x22, 0x41, 0x00, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x00, 0x41, 0x22, 0x14, 0x08, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x3E, 0x41, 0x5D, 0x55, 0x1E, // @
    0x7E, 0x09, 0x09, 0x09, 0x7E, // A
    0x7F, 0x49, 0x49, 0x49, 0x36, // B
    0x3E, 0x41, 0x41, 0x41, 0x22, // C
    0x7F, 0x41, 0x41, 0x22, 0x1C, // D
    0x7F, 0x49, 0x49, 0x49, 0x41, // E
    0x7F, 0x09, 0x09, 0x09, 0x01, // F
    0x3E, 0x41, 0x49, 0x49, 0x7A, // G
    0x7F, 0x08, 0x08, 0x08, 0x7F, // H
    0x00, 0x41, 0x7F, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3F, 0x01, // J
    0x7F, 0x08, 0x14, 0x22, 0x41, // K
    0x7F, 0x40, 0x40, 0x40, 0x40, // L
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // M
    0x7F, 0x04, 0x08, 0x10, 0x7F, // N
    0x3E, 0x41, 0x41, 0x41, 0x3E, // O
    0x7F, 0x09, 0x09, 0x09, 0x06, // P
    0x3E, 0x41, 0x51, 0x21, 0x5E, // Q
    0x7F, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7F, 0x01, 0x01, // T
    0x3F, 0x40, 0x40, 0x40, 0x3F, // U
    0x1F, 0x20, 0x40, 0x20, 0x1F, // V
    0x3F, 0x40, 0x38, 0x40, 0x3F, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x07, 0x08, 0x70, 0x08, 0x07, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x7F, 0x41, 0x41, 0x00, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // backslash
    0x00, 0x41, 0x41, 0x7F, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7F, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7F, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7E, 0x09, 0x01, 0x02, // f
    0x08, 0x54, 0x54, 0x54, 0x3C, // g
    0x7F, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7D, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3D, 0x00, // j
    0x7F, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7F, 0x40, 0x00, // l
    0x7C, 0x04, 0x18, 0x04, 0x78, // m
    0x7C, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7C, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7C, // q
    0x7C, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3F, 0x44, 0x40, 0x20, // t
    0x3C, 0x40, 0x40, 0x20, 0x7C, // u
    0x1C, 0x20, 0x40, 0x20, 0x1C, // v
    0x3C, 0x40, 0x30, 0x40, 0x3C, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0C, 0x50, 0x50, 0x50, 0x3C, // y
    0x44, 0x64, 0x54, 0x4C, 0x44, // z
    0x00, 0x08, 0x36, 0x41, 0x00, // {
    0x00, 0x00, 0x7F, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x08, 0x04, 0x08, 0x10, 0x08, // ~
};
// NOLINTEND(readability-magic-numbers)

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace
{

inline bool inBounds(int16_t x, int16_t y)
{
    return x >= 0 && x < Canvas::WIDTH && y >= 0 && y < Canvas::HEIGHT;
}

} // namespace

// ── Canvas implementation ────────────────────────────────────────────────────

Canvas::Canvas()
    : buffer_{0}
{
}

void Canvas::clear()
{
    std::memset(buffer_, 0, BUFFER_SIZE);
}

const uint8_t *Canvas::buffer() const
{
    return buffer_;
}

// ── Pixel ────────────────────────────────────────────────────────────────────

void Canvas::drawPixel(int16_t x, int16_t y, bool color)
{
    if (!inBounds(x, y))
    {
        return;
    }

    const size_t byteIdx = static_cast<size_t>(y / 8) * static_cast<size_t>(WIDTH) + static_cast<size_t>(x);
    const uint8_t bit = static_cast<uint8_t>(1U << (static_cast<uint8_t>(y) & 7U));

    if (color)
    {
        buffer_[byteIdx] |= bit;
    }
    else
    {
        buffer_[byteIdx] &= static_cast<uint8_t>(~bit);
    }
}

// ── Line (Bresenham) ─────────────────────────────────────────────────────────

void Canvas::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, bool color)
{
    int16_t dx = static_cast<int16_t>(std::abs(x1 - x0));
    int16_t dy = static_cast<int16_t>(-std::abs(y1 - y0));
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;

    for (;;)
    {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        int16_t e2 = static_cast<int16_t>(2 * err);
        if (e2 >= dy)
        {
            err = static_cast<int16_t>(err + dy);
            x0 = static_cast<int16_t>(x0 + sx);
        }
        if (e2 <= dx)
        {
            err = static_cast<int16_t>(err + dx);
            y0 = static_cast<int16_t>(y0 + sy);
        }
    }
}

// ── Rectangle ────────────────────────────────────────────────────────────────

void Canvas::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool color)
{
    drawLine(x, y, static_cast<int16_t>(x + w - 1), y, color);
    drawLine(x, static_cast<int16_t>(y + h - 1), static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1), color);
    drawLine(x, y, x, static_cast<int16_t>(y + h - 1), color);
    drawLine(static_cast<int16_t>(x + w - 1), y, static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + h - 1), color);
}

void Canvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, bool color)
{
    for (int16_t row = y; row < y + h; ++row)
    {
        for (int16_t col = x; col < x + w; ++col)
        {
            drawPixel(col, row, color);
        }
    }
}

// ── Circle (midpoint algorithm) ──────────────────────────────────────────────

void Canvas::drawCircle(int16_t cx, int16_t cy, int16_t r, bool color)
{
    int16_t x = r;
    int16_t y = 0;
    int16_t d = static_cast<int16_t>(1 - r);

    while (x >= y)
    {
        drawPixel(static_cast<int16_t>(cx + x), static_cast<int16_t>(cy + y), color);
        drawPixel(static_cast<int16_t>(cx - x), static_cast<int16_t>(cy + y), color);
        drawPixel(static_cast<int16_t>(cx + x), static_cast<int16_t>(cy - y), color);
        drawPixel(static_cast<int16_t>(cx - x), static_cast<int16_t>(cy - y), color);
        drawPixel(static_cast<int16_t>(cx + y), static_cast<int16_t>(cy + x), color);
        drawPixel(static_cast<int16_t>(cx - y), static_cast<int16_t>(cy + x), color);
        drawPixel(static_cast<int16_t>(cx + y), static_cast<int16_t>(cy - x), color);
        drawPixel(static_cast<int16_t>(cx - y), static_cast<int16_t>(cy - x), color);

        ++y;
        if (d <= 0)
        {
            d = static_cast<int16_t>(d + 2 * y + 1);
        }
        else
        {
            --x;
            d = static_cast<int16_t>(d + 2 * (y - x) + 1);
        }
    }
}

void Canvas::fillCircle(int16_t cx, int16_t cy, int16_t r, bool color)
{
    for (int16_t dy = -r; dy <= r; ++dy)
    {
        for (int16_t dx = -r; dx <= r; ++dx)
        {
            if (dx * dx + dy * dy <= r * r)
            {
                drawPixel(static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy + dy), color);
            }
        }
    }
}

// ── Text ─────────────────────────────────────────────────────────────────────

void Canvas::drawChar(int16_t x, int16_t y, char c, bool color)
{
    if (c < 0x20 || c > 0x7E)
    {
        return;
    }

    const size_t glyphOffset = static_cast<size_t>(c - 0x20) * FONT_WIDTH;

    for (uint8_t col = 0U; col < FONT_WIDTH; ++col)
    {
        uint8_t colData = font5x7[glyphOffset + col];
        for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
        {
            if (colData & (1U << row))
            {
                drawPixel(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row), color);
            }
        }
    }
}

void Canvas::drawStr(int16_t x, int16_t y, const char *text, bool color)
{
    if (text == nullptr)
    {
        return;
    }

    int16_t cursorX = x;
    while (*text != '\0')
    {
        drawChar(cursorX, y, *text, color);
        cursorX = static_cast<int16_t>(cursorX + FONT_WIDTH + CHAR_SPACING);
        ++text;
    }
}

// ── XBM bitmap ───────────────────────────────────────────────────────────────

void Canvas::drawXBM(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bitmap)
{
    if (bitmap == nullptr)
    {
        return;
    }

    const int16_t bytesPerRow = static_cast<int16_t>((w + 7) / 8);

    for (int16_t row = 0; row < h; ++row)
    {
        for (int16_t col = 0; col < w; ++col)
        {
            const size_t byteIdx = static_cast<size_t>(row) * static_cast<size_t>(bytesPerRow) + static_cast<size_t>(col / 8);
            const uint8_t bit = static_cast<uint8_t>(1U << (static_cast<uint8_t>(col) & 7U));

            if (bitmap[byteIdx] & bit)
            {
                drawPixel(static_cast<int16_t>(x + col), static_cast<int16_t>(y + row), true);
            }
        }
    }
}
