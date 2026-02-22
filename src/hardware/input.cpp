#include "hardware/input.h"

#include "config.h"

namespace
{
constexpr uint16_t ADC_MAX = 4095U;
constexpr uint16_t DEADZONE = 600U;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50U;
constexpr TickType_t BUTTON_TASK_DELAY = pdMS_TO_TICKS(10U);
} // namespace

InputManager &InputManager::instance()
{
    static InputManager manager;
    return manager;
}

InputManager::InputManager()
    : buttonTaskHandle_(nullptr),
      buttonPressedEvent_(false)
{
}

bool InputManager::init()
{
    pinMode(PIN_JOY_X, INPUT);
    pinMode(PIN_JOY_Y, INPUT);
    pinMode(PIN_JOY_SW, INPUT_PULLUP);

    if (buttonTaskHandle_ == nullptr)
    {
        BaseType_t result = xTaskCreate(buttonTask, "joy_btn", 2048, this, 1, &buttonTaskHandle_);
        if (result != pdPASS)
        {
            buttonTaskHandle_ = nullptr;
            return false;
        }
    }

    return true;
}

InputManager::InputEvent InputManager::readInput()
{
    if (buttonPressedEvent_)
    {
        buttonPressedEvent_ = false;
        return InputEvent::BUTTON_PRESS;
    }

    const int xRaw = analogRead(PIN_JOY_X);
    const int yRaw = analogRead(PIN_JOY_Y);
    const int center = static_cast<int>(ADC_MAX / 2U);

    if (xRaw < (center - static_cast<int>(DEADZONE)))
    {
        return InputEvent::LEFT;
    }
    if (xRaw > (center + static_cast<int>(DEADZONE)))
    {
        return InputEvent::RIGHT;
    }
    if (yRaw < (center - static_cast<int>(DEADZONE)))
    {
        return InputEvent::UP;
    }
    if (yRaw > (center + static_cast<int>(DEADZONE)))
    {
        return InputEvent::DOWN;
    }

    return InputEvent::CENTER;
}

void InputManager::buttonTask(void *parameter)
{
    auto *self = static_cast<InputManager *>(parameter);
    bool lastStableState = digitalRead(PIN_JOY_SW);
    bool lastReadState = lastStableState;
    uint32_t lastChangeTime = millis();

    while (true)
    {
        const bool currentState = digitalRead(PIN_JOY_SW);

        if (currentState != lastReadState)
        {
            lastReadState = currentState;
            lastChangeTime = millis();
        }

        if ((millis() - lastChangeTime) >= BUTTON_DEBOUNCE_MS && lastStableState != lastReadState)
        {
            lastStableState = lastReadState;
            if (!lastStableState)
            {
                self->buttonPressedEvent_ = true;
            }
        }

        vTaskDelay(BUTTON_TASK_DELAY);
    }
}
