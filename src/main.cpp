/**
 * @file main.cpp
 * @brief HackOS entry point – hardware initialisation and main scheduler loop.
 *
 * Responsibilities:
 *  - Initialises the Serial port at the project-configured baud rate.
 *  - Hands control to the FreeRTOS scheduler; the infinite loop uses
 *    `vTaskDelay` instead of a bare `delay()` so other RTOS tasks receive
 *    CPU time during the idle period.
 *  - Boots the FreeRTOS SystemCore (ThreadManager, HardwareBus mutexes,
 *    MessageBus, and PowerManager).
 *
 * @note All peripheral drivers are initialised in their own modules
 *       (see src/hardware/) and will be wired in during subsequent phases.
 */

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "apps/app_base.h"
#include "apps/file_manager_app.h"
#include "apps/ir_tools_app.h"
#include "apps/launcher_app.h"
#include "apps/nfc_tools_app.h"
#include "apps/rf_tools_app.h"
#include "apps/subghz_analyzer_app.h"
#include "apps/wifi_offensive_app.h"
#include "apps/ble_audit_app.h"
#include "apps/captive_portal_app.h"
#include "apps/amiibo_app.h"
#include "apps/subghz_bruteforcer_app.h"
#include "apps/badbt_app.h"
#include "apps/ghostnet_app.h"
#include "apps/remote_dashboard_app.h"
#include "apps/signal_analyzer_app.h"
#include "apps/plugin_manager_app.h"
#include "apps/wifi_tools_app.h"
#include "config.h"
#include "core/app_manager.h"
#include "core/event_system.h"
#include "core/experience_manager.h"
#include "core/message_bus.h"
#include "core/plugin_manager.h"
#include "core/power_manager.h"
#include "core/state_machine.h"
#include "core/system_core.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/storage.h"
#include "storage/vfs.h"
#include "storage/storage_init.h"

// ── Constants ─────────────────────────────────────────────────────────────────

/// @brief Serial baud rate – must match the `monitor_speed` in platformio.ini.
static constexpr uint32_t SERIAL_BAUD = 115200UL;
static constexpr const char *TAG = "HackOS";

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
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Booting HackOS phase 5 – User-Space SDK");

    // ── FreeRTOS core infrastructure ──────────────────────────────────────
    const bool busOk = hackos::core::HardwareBus::init();
    const bool msgBusOk = hackos::core::MessageBus::instance().init();
    const bool powerOk = hackos::core::PowerManager::instance().init(PIN_JOY_SW);

    ESP_LOGI(TAG, "HardwareBus init: %s", busOk ? "OK" : "FAIL");
    ESP_LOGI(TAG, "MessageBus init: %s", msgBusOk ? "OK" : "FAIL");
    ESP_LOGI(TAG, "PowerManager init: %s", powerOk ? "OK" : "FAIL");

    // ── Legacy HAL initialisation ─────────────────────────────────────────
    const bool displayOk = DisplayManager::instance().init();
    const bool inputOk = InputManager::instance().init();
    const bool storageOk = StorageManager::instance().mount();
    const bool eventSystemOk = EventSystem::instance().init();
    const bool appManagerOk = AppManager::instance().init();
    ESP_LOGD(TAG, "HAL instances acquired and initialized");

    ESP_LOGI(TAG, "DisplayManager init: %s", displayOk ? "OK" : "FAIL");
    ESP_LOGI(TAG, "InputManager init: %s", inputOk ? "OK" : "FAIL");
    ESP_LOGI(TAG, "StorageManager mount: %s (%s)", storageOk ? "OK" : "FAIL", StorageManager::instance().lastError());

    // ── VFS & folder structure ────────────────────────────────────────────
    const bool vfsOk = hackos::storage::VirtualFS::instance().init();
    ESP_LOGI(TAG, "VirtualFS init: %s (SD=%s, Flash=%s)", vfsOk ? "OK" : "FAIL",
             hackos::storage::VirtualFS::instance().sdMounted() ? "yes" : "no",
             hackos::storage::VirtualFS::instance().flashMounted() ? "yes" : "no");

    const bool foldersOk = hackos::storage::StorageInit::ensureFolderStructure();
    ESP_LOGI(TAG, "SD folder structure: %s", foldersOk ? "OK" : "INCOMPLETE");

    // ── Gamification – ExperienceManager ─────────────────────────────────
    const bool xpOk = ExperienceManager::instance().init();
    ESP_LOGI(TAG, "ExperienceManager init: %s", xpOk ? "OK" : "FAIL");

    ESP_LOGI(TAG, "EventSystem init: %s", eventSystemOk ? "OK" : "FAIL");
    ESP_LOGI(TAG, "AppManager init: %s", appManagerOk ? "OK" : "FAIL");

    StateMachine::instance().init(GlobalState::BOOT);
    (void)StateMachine::instance().pushState(GlobalState::SPLASH);
    (void)StateMachine::instance().pushState(GlobalState::LAUNCHER);
    ESP_LOGI(TAG, "StateMachine state: %d", static_cast<int>(StateMachine::instance().currentState()));

    (void)AppManager::instance().registerApp("launcher", createLauncherApp);
    (void)AppManager::instance().registerApp("wifi_tools", createWifiToolsApp);
    (void)AppManager::instance().registerApp("ir_tools", createIRToolsApp);
    (void)AppManager::instance().registerApp("nfc_tools", createNFCToolsApp);
    (void)AppManager::instance().registerApp("rf_tools", createRFToolsApp);
    (void)AppManager::instance().registerApp("file_manager", createFileManagerApp);
    (void)AppManager::instance().registerApp("subghz_analyzer", createSubGhzAnalyzerApp);
    (void)AppManager::instance().registerApp("wifi_offensive", createWifiOffensiveApp);
    (void)AppManager::instance().registerApp("captive_portal", createCaptivePortalApp);
    (void)AppManager::instance().registerApp("ble_audit", createBleAuditApp);
    (void)AppManager::instance().registerApp("amiibo", createAmiiboApp);
    (void)AppManager::instance().registerApp("subghz_bf", createSubGhzBruteforceApp);
    (void)AppManager::instance().registerApp("badbt", createBadBtApp);
    (void)AppManager::instance().registerApp("ghostnet", createGhostNetApp);
    (void)AppManager::instance().registerApp("remote_dashboard", createRemoteDashboardApp);
    (void)AppManager::instance().registerApp("signal_analyzer", createSignalAnalyzerApp);
    (void)AppManager::instance().registerApp("plugin_manager", createPluginManagerApp);

    // ── Dynamic Plugin Loading ───────────────────────────────────────────
    {
        auto &pm = hackos::core::PluginManager::instance();
        const size_t loaded = pm.scanAndLoad();
        const size_t registered = pm.registerAll();
        ESP_LOGI(TAG, "Plugins: %u loaded, %u registered",
                 static_cast<unsigned>(loaded), static_cast<unsigned>(registered));
    }

    (void)AppManager::instance().launchApp("launcher");

    const uint32_t heapSize = ESP.getHeapSize();
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t usedPercent = (heapSize > 0U)
                                     ? static_cast<uint32_t>((static_cast<uint64_t>(heapSize - freeHeap) * 100ULL) / heapSize)
                                     : 0U;
    ESP_LOGI(TAG, "RAM usage: %lu%% (free=%lu, total=%lu)", static_cast<unsigned long>(usedPercent),
             static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(heapSize));
}

/**
 * @brief Cooperative main loop, yielding to FreeRTOS every 10 ms.
 *
 * Using `vTaskDelay` instead of `delay()` allows the FreeRTOS idle task
 * and any background tasks to execute during the sleep window.
 */
void loop()
{
    EventSystem::instance().dispatchPendingEvents();
    AppManager::instance().loop();

    // Let the PowerManager evaluate idle-timeout / sleep policy.
    hackos::core::PowerManager::instance().tick();

    vTaskDelay(LOOP_DELAY_TICKS);
}
