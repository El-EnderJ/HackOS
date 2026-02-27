#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <cstddef>
#include <cstdint>

class DisplayManager
{
public:
    static DisplayManager &instance();

    bool init();
    void clear();
    void drawPixel(int16_t x, int16_t y, uint16_t color = SSD1306_WHITE);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color = SSD1306_WHITE);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color = SSD1306_WHITE);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color = SSD1306_WHITE);
    void drawText(int16_t x, int16_t y, const char *text, uint8_t textSize = 1U, uint16_t color = SSD1306_WHITE);
    void present();
    bool isInitialized() const;

    /// @brief Direct access to the SSD1306 display buffer for DMA-style writes.
    uint8_t *getDisplayBuffer();

private:
    static constexpr int16_t WIDTH = 128;
    static constexpr int16_t HEIGHT = 64;
    static constexpr size_t BUFFER_SIZE = static_cast<size_t>(WIDTH) * static_cast<size_t>(HEIGHT) / 8U;

    DisplayManager();

    Adafruit_SSD1306 display_;
    GFXcanvas1 backBuffer_;
    uint8_t frontBuffer_[BUFFER_SIZE];
    bool initialized_;
};
