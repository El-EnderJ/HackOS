#include "ui/widgets.h"

#include <Arduino.h>
#include <cstdio>

#include "hardware/display.h"

MenuListView::MenuListView(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t visibleRows)
    : Widget(x, y, width, height),
      items_(nullptr),
      itemCount_(0U),
      selectedIndex_(0U),
      firstVisible_(0U),
      visibleRows_(visibleRows)
{
}

void MenuListView::setItems(const char *const *items, size_t itemCount)
{
    items_ = items;
    itemCount_ = itemCount;
    selectedIndex_ = 0U;
    firstVisible_ = 0U;
    markDirty();
}

void MenuListView::moveSelection(int8_t delta)
{
    if (itemCount_ == 0U || delta == 0)
    {
        return;
    }

    const int32_t next = constrain(static_cast<int32_t>(selectedIndex_) + static_cast<int32_t>(delta), 0L, static_cast<long>(itemCount_ - 1U));
    const size_t nextIndex = static_cast<size_t>(next);
    if (nextIndex == selectedIndex_)
    {
        return;
    }

    selectedIndex_ = nextIndex;
    if (selectedIndex_ < firstVisible_)
    {
        firstVisible_ = selectedIndex_;
    }
    else if (selectedIndex_ >= (firstVisible_ + visibleRows_))
    {
        firstVisible_ = selectedIndex_ - visibleRows_ + 1U;
    }

    markDirty();
}

size_t MenuListView::selectedIndex() const
{
    return selectedIndex_;
}

size_t MenuListView::itemCount() const
{
    return itemCount_;
}

void MenuListView::draw()
{
    if (items_ == nullptr)
    {
        return;
    }

    char line[24];
    for (uint8_t row = 0U; row < visibleRows_; ++row)
    {
        const size_t index = firstVisible_ + row;
        if (index >= itemCount_)
        {
            break;
        }

        std::snprintf(line, sizeof(line), "%c %s", index == selectedIndex_ ? '>' : ' ', items_[index]);
        DisplayManager::instance().drawText(x_, y_ + static_cast<int16_t>(row * 10), line);
    }
}

StatusBar::StatusBar(int16_t x, int16_t y, int16_t width, int16_t height)
    : Widget(x, y, width, height),
      sdMounted_(false),
      wifiConnected_(false),
      batteryPercent_(100U),
      hours_(12U),
      minutes_(0U)
{
}

void StatusBar::setConnectivity(bool sdMounted, bool wifiConnected)
{
    if (sdMounted_ == sdMounted && wifiConnected_ == wifiConnected)
    {
        return;
    }

    sdMounted_ = sdMounted;
    wifiConnected_ = wifiConnected;
    markDirty();
}

void StatusBar::setBatteryLevel(uint8_t percent)
{
    const uint8_t value = percent > 100U ? 100U : percent;
    if (batteryPercent_ == value)
    {
        return;
    }

    batteryPercent_ = value;
    markDirty();
}

void StatusBar::setTime(uint8_t hours, uint8_t minutes)
{
    if (hours_ == hours && minutes_ == minutes)
    {
        return;
    }

    hours_ = hours;
    minutes_ = minutes;
    markDirty();
}

void StatusBar::draw()
{
    char statusLine[32];
    std::snprintf(statusLine, sizeof(statusLine), "%s %s %02u:%02u", sdMounted_ ? "SD" : "--", wifiConnected_ ? "WF" : "--",
                  static_cast<unsigned>(hours_), static_cast<unsigned>(minutes_));
    DisplayManager::instance().drawText(x_, y_, statusLine);
    DisplayManager::instance().drawRect(x_ + width_ - 20, y_, 18, 8);
    const int16_t fillWidth = static_cast<int16_t>((static_cast<uint16_t>(batteryPercent_) * 16U) / 100U);
    DisplayManager::instance().fillRect(x_ + width_ - 19, y_ + 1, fillWidth, 6);
}

ProgressBar::ProgressBar(int16_t x, int16_t y, int16_t width, int16_t height)
    : Widget(x, y, width, height),
      percent_(0U)
{
}

void ProgressBar::setProgress(uint8_t percent)
{
    const uint8_t value = percent > 100U ? 100U : percent;
    if (percent_ == value)
    {
        return;
    }

    percent_ = value;
    markDirty();
}

void ProgressBar::draw()
{
    DisplayManager::instance().drawRect(x_, y_, width_, height_);
    const int16_t innerWidth = static_cast<int16_t>((static_cast<uint16_t>(percent_) * static_cast<uint16_t>(width_ - 2)) / 100U);
    DisplayManager::instance().fillRect(x_ + 1, y_ + 1, innerWidth, height_ - 2);
}

DialogBox::DialogBox(int16_t x, int16_t y, int16_t width, int16_t height)
    : Widget(x, y, width, height),
      visible_(false),
      title_(""),
      message_("")
{
}

void DialogBox::setVisible(bool visible)
{
    if (visible_ == visible)
    {
        return;
    }

    visible_ = visible;
    markDirty();
}

bool DialogBox::isVisible() const
{
    return visible_;
}

void DialogBox::setText(const char *title, const char *message)
{
    title_ = title == nullptr ? "" : title;
    message_ = message == nullptr ? "" : message;
    markDirty();
}

void DialogBox::draw()
{
    if (!visible_)
    {
        return;
    }

    DisplayManager::instance().drawRect(x_, y_, width_, height_);
    DisplayManager::instance().drawText(x_ + 2, y_ + 2, title_);
    DisplayManager::instance().drawLine(x_ + 1, y_ + 10, x_ + width_ - 2, y_ + 10);
    DisplayManager::instance().drawText(x_ + 2, y_ + 14, message_);
}
