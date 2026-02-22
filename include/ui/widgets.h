#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/widget.h"

class MenuListView : public Widget
{
public:
    MenuListView(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t visibleRows);
    void setItems(const char *const *items, size_t itemCount);
    void moveSelection(int8_t delta);
    size_t selectedIndex() const;
    size_t itemCount() const;
    void draw() override;

private:
    const char *const *items_;
    size_t itemCount_;
    size_t selectedIndex_;
    size_t firstVisible_;
    uint8_t visibleRows_;
};

class StatusBar : public Widget
{
public:
    StatusBar(int16_t x, int16_t y, int16_t width, int16_t height);
    void setConnectivity(bool sdMounted, bool wifiConnected);
    void setBatteryLevel(uint8_t percent);
    void setTime(uint8_t hours, uint8_t minutes);
    void draw() override;

private:
    bool sdMounted_;
    bool wifiConnected_;
    uint8_t batteryPercent_;
    uint8_t hours_;
    uint8_t minutes_;
};

class ProgressBar : public Widget
{
public:
    ProgressBar(int16_t x, int16_t y, int16_t width, int16_t height);
    void setProgress(uint8_t percent);
    void draw() override;

private:
    uint8_t percent_;
};

class DialogBox : public Widget
{
public:
    DialogBox(int16_t x, int16_t y, int16_t width, int16_t height);
    void setVisible(bool visible);
    bool isVisible() const;
    void setText(const char *title, const char *message);
    void draw() override;

private:
    bool visible_;
    const char *title_;
    const char *message_;
};
