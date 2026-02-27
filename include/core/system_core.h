/**
 * @file system_core.h
 * @brief Thread management and hardware-bus mutexes for HackOS.
 *
 * ThreadManager  – creates the four main FreeRTOS tasks using
 *                  xTaskCreateStatic (zero heap fragmentation).
 *
 * HardwareBus    – strict mutexes for the shared I2C and SPI buses so
 *                  no app can corrupt ongoing transfers.
 *
 * @note Everything lives in the hackos::core namespace.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace hackos::core {

// ── HardwareBus ──────────────────────────────────────────────────────────────

/**
 * @brief RAII-friendly mutex wrappers for the shared peripheral buses.
 *
 * Usage:
 *   if (HardwareBus::i2c().acquire()) {
 *       // ... critical I2C transaction ...
 *       HardwareBus::i2c().release();
 *   }
 */
class BusMutex
{
public:
    BusMutex();

    /// @brief Initialise the underlying static mutex.
    bool init();

    /**
     * @brief Acquire the bus mutex (blocks up to @p ticksToWait).
     * @return true if acquired.
     */
    bool acquire(TickType_t ticksToWait = pdMS_TO_TICKS(1000U));

    /// @brief Release the bus mutex.
    void release();

private:
    SemaphoreHandle_t handle_;
    StaticSemaphore_t buffer_;
};

/**
 * @brief Central access point for all hardware-bus mutexes.
 *
 * Call HardwareBus::init() once during boot.  Then use i2c() / spi()
 * to obtain the relevant mutex before touching a shared bus.
 */
class HardwareBus
{
public:
    /// @brief Initialise all bus mutexes.  Call once.
    static bool init();

    /// @brief Mutex guarding the I2C bus (OLED display).
    static BusMutex &i2c();

    /// @brief Mutex guarding the SPI bus (SD card, NFC module).
    static BusMutex &spi();

private:
    HardwareBus() = delete;
};

// ── ThreadManager ────────────────────────────────────────────────────────────

/// Task function signature expected by ThreadManager.
using TaskFunction = void (*)(void *);

/**
 * @brief Manages the four main HackOS FreeRTOS tasks.
 *
 * All tasks are created with xTaskCreateStatic to keep the heap
 * deterministic.  Stack sizes are tuned per subsystem.
 */
class ThreadManager
{
public:
    static ThreadManager &instance();

    /**
     * @brief Create the four core tasks.
     *
     * @param guiFunc   GUI_Task  entry point  (draws the display).
     * @param radioFunc Radio_Task entry point  (HW RX/TX).
     * @param coreFunc  Core_Task entry point   (application logic).
     * @param ioFunc    IO_Task   entry point   (SD / filesystem).
     * @return true if every task was created successfully.
     */
    bool startAll(TaskFunction guiFunc,
                  TaskFunction radioFunc,
                  TaskFunction coreFunc,
                  TaskFunction ioFunc);

    /// @brief True once startAll() has completed successfully.
    bool isRunning() const;

    /// @brief Obtain the handle for a named subsystem (0-3).
    TaskHandle_t taskHandle(size_t index) const;

private:
    /// Descriptive names visible in RTOS tooling.
    static constexpr const char *TASK_NAMES[] = {
        "GUI_Task",
        "Radio_Task",
        "Core_Task",
        "IO_Task",
    };

    /// Number of subsystem tasks.
    static constexpr size_t NUM_TASKS = 4U;

    /// Stack sizes in bytes (StackType_t is uint8_t on ESP-IDF / Arduino).
    static constexpr uint32_t GUI_STACK   = 4096U;
    static constexpr uint32_t RADIO_STACK = 4096U;
    static constexpr uint32_t CORE_STACK  = 4096U;
    static constexpr uint32_t IO_STACK    = 4096U;

    /// Task priorities (higher = more urgent).
    static constexpr UBaseType_t GUI_PRIO   = 3U;
    static constexpr UBaseType_t RADIO_PRIO = 4U;
    static constexpr UBaseType_t CORE_PRIO  = 2U;
    static constexpr UBaseType_t IO_PRIO    = 1U;

    ThreadManager();

    /// Static stacks – one per task, sized individually.
    StackType_t guiStack_[GUI_STACK];
    StackType_t radioStack_[RADIO_STACK];
    StackType_t coreStack_[CORE_STACK];
    StackType_t ioStack_[IO_STACK];

    /// Static task control blocks.
    StaticTask_t tcb_[NUM_TASKS];

    /// Runtime handles returned by xTaskCreateStatic.
    TaskHandle_t handles_[NUM_TASKS];

    bool running_;
};

} // namespace hackos::core
