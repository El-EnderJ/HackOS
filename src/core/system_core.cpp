/**
 * @file system_core.cpp
 * @brief ThreadManager and HardwareBus mutex implementation.
 */

#include "core/system_core.h"

#include <esp_log.h>

static constexpr const char *TAG = "SystemCore";

namespace hackos::core {

// ═══════════════════════════════════════════════════════════════════════════════
// BusMutex
// ═══════════════════════════════════════════════════════════════════════════════

BusMutex::BusMutex()
    : handle_(nullptr),
      buffer_{}
{
}

bool BusMutex::init()
{
    if (handle_ != nullptr)
    {
        return true;
    }

    handle_ = xSemaphoreCreateMutexStatic(&buffer_);
    return handle_ != nullptr;
}

bool BusMutex::acquire(TickType_t ticksToWait)
{
    if (handle_ == nullptr)
    {
        return false;
    }

    return xSemaphoreTake(handle_, ticksToWait) == pdTRUE;
}

void BusMutex::release()
{
    if (handle_ != nullptr)
    {
        xSemaphoreGive(handle_);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HardwareBus  (static storage)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {
BusMutex g_i2cMutex;
BusMutex g_spiMutex;
} // namespace

bool HardwareBus::init()
{
    const bool i2cOk = g_i2cMutex.init();
    const bool spiOk = g_spiMutex.init();

    if (!i2cOk || !spiOk)
    {
        ESP_LOGE(TAG, "HardwareBus mutex init failed (I2C=%d, SPI=%d)", i2cOk, spiOk);
    }
    else
    {
        ESP_LOGI(TAG, "HardwareBus mutexes initialised (I2C + SPI)");
    }

    return i2cOk && spiOk;
}

BusMutex &HardwareBus::i2c() { return g_i2cMutex; }
BusMutex &HardwareBus::spi() { return g_spiMutex; }

// ═══════════════════════════════════════════════════════════════════════════════
// ThreadManager
// ═══════════════════════════════════════════════════════════════════════════════

ThreadManager &ThreadManager::instance()
{
    static ThreadManager mgr;
    return mgr;
}

ThreadManager::ThreadManager()
    : guiStack_{},
      radioStack_{},
      coreStack_{},
      ioStack_{},
      tcb_{},
      handles_{},
      running_(false)
{
}

bool ThreadManager::startAll(TaskFunction guiFunc,
                             TaskFunction radioFunc,
                             TaskFunction coreFunc,
                             TaskFunction ioFunc)
{
    if (running_)
    {
        ESP_LOGW(TAG, "ThreadManager: tasks already running");
        return true;
    }

    struct TaskDef
    {
        TaskFunction func;
        const char *name;
        StackType_t *stack;
        uint32_t stackSize;
        UBaseType_t priority;
    };

    const TaskDef defs[NUM_TASKS] = {
        {guiFunc,   TASK_NAMES[0], guiStack_,   GUI_STACK,   GUI_PRIO},
        {radioFunc, TASK_NAMES[1], radioStack_, RADIO_STACK, RADIO_PRIO},
        {coreFunc,  TASK_NAMES[2], coreStack_,  CORE_STACK,  CORE_PRIO},
        {ioFunc,    TASK_NAMES[3], ioStack_,    IO_STACK,    IO_PRIO},
    };

    for (size_t i = 0; i < NUM_TASKS; ++i)
    {
        handles_[i] = xTaskCreateStatic(
            defs[i].func,
            defs[i].name,
            defs[i].stackSize,
            nullptr,            // pvParameters
            defs[i].priority,
            defs[i].stack,
            &tcb_[i]);

        if (handles_[i] == nullptr)
        {
            ESP_LOGE(TAG, "Failed to create task: %s", defs[i].name);
            return false;
        }

        ESP_LOGI(TAG, "Created task: %s (prio=%u, stack=%lu words)",
                 defs[i].name,
                 static_cast<unsigned>(defs[i].priority),
                 static_cast<unsigned long>(defs[i].stackSize));
    }

    running_ = true;
    return true;
}

bool ThreadManager::isRunning() const
{
    return running_;
}

TaskHandle_t ThreadManager::taskHandle(size_t index) const
{
    if (index >= NUM_TASKS)
    {
        return nullptr;
    }

    return handles_[index];
}

} // namespace hackos::core
