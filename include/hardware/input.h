#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class InputManager
{
public:
    enum class InputEvent
    {
        NONE,
        UP,
        DOWN,
        LEFT,
        RIGHT,
        CENTER,
        BUTTON_PRESS,
    };

    static InputManager &instance();

    bool init();
    InputEvent readInput();

private:
    static void buttonTask(void *parameter);

    InputManager();

    TaskHandle_t buttonTaskHandle_;
    volatile bool buttonPressedEvent_;
};
