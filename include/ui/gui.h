/**
 * @file gui.h
 * @brief Render-loop manager for the HackOS UI framework.
 *
 * Gui owns a Canvas and a FreeRTOS task that runs at ~30 FPS.
 * The task blocks on a task-notification with a 33 ms timeout;
 * when it wakes it asks the active ViewDispatcher to draw onto
 * the Canvas, then pushes the buffer to the OLED via
 * DisplayManager.
 */

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ui/canvas.h"

class ViewDispatcher;

class Gui
{
public:
    static Gui &instance();

    /**
     * @brief Create the render-loop FreeRTOS task.
     * @return true if the task was created successfully.
     */
    bool init();

    /**
     * @brief Attach a ViewDispatcher whose active view will be drawn.
     * @param dispatcher  Pointer (ownership remains with caller).
     */
    void setViewDispatcher(ViewDispatcher *dispatcher);

    /**
     * @brief Send a notification to the render task so it wakes up
     *        immediately instead of waiting for the next 33 ms tick.
     */
    void requestRedraw();

private:
    /// ~30 FPS frame period.
    static constexpr TickType_t FRAME_PERIOD_TICKS = pdMS_TO_TICKS(33U);
    static constexpr uint32_t GUI_STACK_SIZE = 4096U;
    static constexpr UBaseType_t GUI_PRIORITY = 3U;

    Gui();

    /// FreeRTOS task entry point.
    static void renderTask(void *param);

    Canvas canvas_;
    ViewDispatcher *dispatcher_;
    TaskHandle_t taskHandle_;
};
