#pragma once

#include <cstdint>

class Widget
{
public:
    Widget(int16_t x, int16_t y, int16_t width, int16_t height)
        : x_(x),
          y_(y),
          width_(width),
          height_(height),
          isDirty_(true)
    {
    }

    virtual ~Widget() = default;
    virtual void draw() = 0;

    bool isDirty() const { return isDirty_; }
    void markDirty() { isDirty_ = true; }
    void clearDirty() { isDirty_ = false; }

protected:
    int16_t x_;
    int16_t y_;
    int16_t width_;
    int16_t height_;
    bool isDirty_;
};
