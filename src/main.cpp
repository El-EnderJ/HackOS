/**
 * @file main.cpp
 * @brief HackOS entry point – hardware initialisation and main scheduler loop.
 *
 * Responsibilities:
 *  - Initialises the Serial port at the project-configured baud rate.
 *  - Hands control to the FreeRTOS scheduler; the infinite loop uses
 *    `vTaskDelay` instead of a bare `delay()` so other RTOS tasks receive
 *    CPU time during the idle period.
 *
 * @note All peripheral drivers are initialised in their own modules
 *       (see src/hardware/) and will be wired in during subsequent phases.
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

// ── Constants ─────────────────────────────────────────────────────────────────

/// @brief Serial baud rate – must match the `monitor_speed` in platformio.ini.
static constexpr uint32_t SERIAL_BAUD = 115200UL;

/// @brief Main-loop idle period expressed in FreeRTOS ticks (10 ms).
static constexpr TickType_t LOOP_DELAY_TICKS = pdMS_TO_TICKS(10U);

// ── Arduino lifecycle ─────────────────────────────────────────────────────────

/**
 * @brief One-time hardware and subsystem initialisation.
 *
 * Called by the Arduino/ESP-IDF startup code before the scheduler loop
 * begins.  Peripheral drivers will be added here in later phases.
 */
void setup()
{
    Serial.begin(SERIAL_BAUD);
    Serial.println(F("[HackOS] Boot – phase 1 scaffolding OK"));
}

/**
 * @brief Cooperative main loop, yielding to FreeRTOS every 10 ms.
 *
 * Using `vTaskDelay` instead of `delay()` allows the FreeRTOS idle task
 * and any background tasks to execute during the sleep window.
 */
void loop()
{
    vTaskDelay(LOOP_DELAY_TICKS);
}
