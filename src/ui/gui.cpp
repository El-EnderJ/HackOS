/**
 * @file gui.cpp
 * @brief Gui render-loop implementation.
 *
 * The FreeRTOS task blocks with ulTaskNotifyTake (timeout = 33 ms).
 * On wake-up it clears the canvas, asks the ViewDispatcher to draw,
 * and pushes the resulting buffer to the OLED through DisplayManager.
 */

#include "ui/gui.h"

#include <cstring>

#include "hardware/display.h"
#include "ui/canvas.h"
#include "ui/view_dispatcher.h"

Gui &Gui::instance()
{
    static Gui gui;
    return gui;
}

Gui::Gui()
    : canvas_(),
      dispatcher_(nullptr),
      taskHandle_(nullptr)
{
}

bool Gui::init()
{
    if (taskHandle_ != nullptr)
    {
        return true; // already running
    }

    BaseType_t result = xTaskCreate(
        renderTask,
        "GUI_Render",
        GUI_STACK_SIZE,
        this,
        GUI_PRIORITY,
        &taskHandle_);

    return result == pdPASS;
}

void Gui::setViewDispatcher(ViewDispatcher *dispatcher)
{
    dispatcher_ = dispatcher;
}

void Gui::requestRedraw()
{
    if (taskHandle_ != nullptr)
    {
        xTaskNotifyGive(taskHandle_);
    }
}

void Gui::renderTask(void *param)
{
    Gui *self = static_cast<Gui *>(param);

    for (;;)
    {
        // Block until notified or 33 ms timeout (~30 FPS).
        (void)ulTaskNotifyTake(pdTRUE, FRAME_PERIOD_TICKS);

        // Draw current view into the off-screen canvas.
        self->canvas_.clear();
        if (self->dispatcher_ != nullptr)
        {
            self->dispatcher_->draw(&self->canvas_);
        }

        // Push the canvas buffer to the OLED display.
        DisplayManager &dm = DisplayManager::instance();
        if (dm.isInitialized())
        {
            const uint8_t *src = self->canvas_.buffer();
            uint8_t *dst = dm.getDisplayBuffer();
            if (dst != nullptr)
            {
                std::memcpy(dst, src, Canvas::BUFFER_SIZE);
                dm.present();
            }
        }
    }
}
