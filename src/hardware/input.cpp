#include "hardware/input.h"

#include "config.h"

namespace
{
constexpr uint16_t ADC_MAX = 4095U;
constexpr uint16_t DEADZONE = 600U;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50U;
constexpr TickType_t BUTTON_TASK_DELAY = pdMS_TO_TICKS(10U);
constexpr uint32_t BUTTON_TASK_STACK_SIZE = 2048U;
constexpr UBaseType_t BUTTON_TASK_PRIORITY = 1U;
} // namespace

InputManager &InputManager::instance()
{
    static InputManager manager;
    return manager;
}

InputManager::InputManager()
    : buttonTaskHandle_(nullptr),
      buttonEventQueue_(nullptr)
{
}

bool InputManager::init()
{
    pinMode(PIN_JOY_X, INPUT);
    pinMode(PIN_JOY_Y, INPUT);
    pinMode(PIN_JOY_SW, INPUT_PULLUP);

    if (buttonEventQueue_ == nullptr)
    {
        buttonEventQueue_ = xQueueCreate(4, sizeof(uint8_t));
        if (buttonEventQueue_ == nullptr)
        {
            return false;
        }
    }

    if (buttonTaskHandle_ == nullptr)
    {
        BaseType_t result = xTaskCreate(buttonTask, "joy_btn", BUTTON_TASK_STACK_SIZE, this, BUTTON_TASK_PRIORITY, &buttonTaskHandle_);
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
    uint8_t buttonEvent = 0U;
    if (buttonEventQueue_ != nullptr && xQueueReceive(buttonEventQueue_, &buttonEvent, 0) == pdTRUE)
    {
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
                const uint8_t event = 1U;
                (void)xQueueSend(self->buttonEventQueue_, &event, 0);
            }
        }

        vTaskDelay(BUTTON_TASK_DELAY);
    }
}
