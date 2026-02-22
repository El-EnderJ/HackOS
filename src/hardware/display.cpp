#include "hardware/display.h"

#include <Wire.h>
#include <cstring>

#include "config.h"

DisplayManager &DisplayManager::instance()
{
    static DisplayManager manager;
    return manager;
}

DisplayManager::DisplayManager()
    : display_(WIDTH, HEIGHT, &Wire, -1),
      backBuffer_(WIDTH, HEIGHT),
      frontBuffer_{0},
      initialized_(false)
{
}

bool DisplayManager::init()
{
    if (initialized_)
    {
        return true;
    }

    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    if (!display_.begin(SSD1306_SWITCHCAPVCC, 0x3CU))
    {
        return false;
    }

    backBuffer_.fillScreen(SSD1306_BLACK);
    std::memset(frontBuffer_, 0, sizeof(frontBuffer_));
    initialized_ = true;
    present();
    return true;
}

void DisplayManager::clear()
{
    backBuffer_.fillScreen(SSD1306_BLACK);
}

void DisplayManager::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    backBuffer_.drawPixel(x, y, color);
}

void DisplayManager::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    backBuffer_.drawLine(x0, y0, x1, y1, color);
}

void DisplayManager::drawText(int16_t x, int16_t y, const char *text, uint8_t textSize, uint16_t color)
{
    if (text == nullptr)
    {
        return;
    }

    backBuffer_.setCursor(x, y);
    backBuffer_.setTextColor(color);
    backBuffer_.setTextSize(textSize);
    backBuffer_.print(text);
}

void DisplayManager::present()
{
    if (!initialized_)
    {
        return;
    }

    std::memcpy(frontBuffer_, backBuffer_.getBuffer(), sizeof(frontBuffer_));
    std::memcpy(display_.getBuffer(), frontBuffer_, sizeof(frontBuffer_));
    display_.display();
}

bool DisplayManager::isInitialized() const
{
    return initialized_;
}
