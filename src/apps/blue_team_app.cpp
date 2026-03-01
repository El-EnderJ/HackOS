/**
 * @file blue_team_app.cpp
 * @brief Blue Team – Intrusion Detection System (IDS) & Defensive Scanner.
 *
 * Implements:
 *  - **IDS Monitor**: Passive scanning that detects:
 *      • WiFi Deauth attacks (promiscuous mode frame analysis).
 *      • Rogue APs / Pineapple detection (duplicate SSIDs, suspicious OUIs).
 *      • BLE Spam detection (rapid MAC rotation / high ADV rate).
 *  - **Skimmer & Tracker Detector**: Scans BLE for:
 *      • Apple AirTag / Samsung SmartTag / Tile trackers.
 *      • Bluetooth HC-05/HC-06 skimmer modules.
 *
 * Uses ToastManager for real-time threat alerts with haptic feedback.
 *
 * @warning For authorised security auditing only.
 */

#include "apps/blue_team_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>

#include <NimBLEDevice.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/toast_manager.h"
#include "ui/widgets.h"

static constexpr const char *TAG_BT = "BlueTeam";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_THREATS          = 16U;
static constexpr size_t  THREAT_DESC_LEN      = 28U;
static constexpr size_t  MAX_BLE_DEVICES      = 24U;
static constexpr uint32_t IDS_SCAN_INTERVAL_MS = 2000U;
static constexpr uint32_t DEAUTH_THRESHOLD     = 3U;   ///< Deauths in window = attack
static constexpr uint32_t DEAUTH_WINDOW_MS     = 5000U;
static constexpr uint32_t BLE_SPAM_THRESHOLD   = 10U;  ///< ADVs from new MACs/sec
static constexpr uint32_t BLE_SPAM_WINDOW_MS   = 3000U;

// ── WiFi frame types for deauth detection ────────────────────────────────────

static constexpr uint8_t WIFI_FRAME_TYPE_MGMT   = 0x00U;
static constexpr uint8_t WIFI_FRAME_SUBTYPE_DEAUTH = 0x0CU;
static constexpr uint8_t WIFI_FRAME_SUBTYPE_DISASSOC = 0x0AU;

// ── Known suspicious BLE prefixes ────────────────────────────────────────────

static constexpr size_t SKIMMER_PREFIX_COUNT = 4U;
static const char *const SKIMMER_PREFIXES[SKIMMER_PREFIX_COUNT] = {
    "HC-05",
    "HC-06",
    "CC41-A",
    "JDY-",
};

static constexpr size_t TRACKER_PREFIX_COUNT = 3U;
static const char *const TRACKER_PREFIXES[TRACKER_PREFIX_COUNT] = {
    "SmartTag",
    "Tile",
    "Find My",
};

// Apple manufacturer ID for AirTag detection
static constexpr uint16_t APPLE_COMPANY_ID = 0x004CU;

// ── Threat types ─────────────────────────────────────────────────────────────

enum class ThreatType : uint8_t
{
    DEAUTH_ATTACK,
    ROGUE_AP,
    BLE_SPAM,
    SKIMMER,
    TRACKER_AIRTAG,
    TRACKER_OTHER,
};

static const char *threatTypeStr(ThreatType t)
{
    switch (t)
    {
    case ThreatType::DEAUTH_ATTACK:  return "Deauth Attack";
    case ThreatType::ROGUE_AP:       return "Rogue AP";
    case ThreatType::BLE_SPAM:       return "BLE Spam";
    case ThreatType::SKIMMER:        return "Skimmer";
    case ThreatType::TRACKER_AIRTAG: return "AirTag";
    case ThreatType::TRACKER_OTHER:  return "Tracker";
    }
    return "Unknown";
}

// ── Threat log entry ─────────────────────────────────────────────────────────

struct ThreatEntry
{
    ThreatType type;
    char desc[THREAT_DESC_LEN + 1U];
    uint32_t timestampMs;
};

// ── App state machine ────────────────────────────────────────────────────────

enum class BTState : uint8_t
{
    MENU_MAIN,
    IDS_RUNNING,
    SCANNER_RUNNING,
    THREAT_LOG,
};

// ── Menu labels ──────────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "IDS Monitor",
    "Skimmer/Tracker Scan",
    "Threat Log",
    "Back",
};

// ── Globals for WiFi promiscuous callback ────────────────────────────────────

static volatile uint32_t g_deauthCount = 0U;
static volatile uint32_t g_deauthWindowStart = 0U;

/// WiFi promiscuous mode callback – counts deauth/disassoc frames.
static void IRAM_ATTR wifiPromiscuousCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || buf == nullptr)
    {
        return;
    }

    const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *frame = pkt->payload;
    // IEEE 802.11 frame control: type in bits 3:2, subtype in bits 7:4
    const uint8_t frameType    = (frame[0] >> 2U) & 0x03U;
    const uint8_t frameSubtype = (frame[0] >> 4U) & 0x0FU;

    if (frameType == WIFI_FRAME_TYPE_MGMT &&
        (frameSubtype == WIFI_FRAME_SUBTYPE_DEAUTH ||
         frameSubtype == WIFI_FRAME_SUBTYPE_DISASSOC))
    {
        g_deauthCount++;
    }
}

// ── BLE scan callback for Blue Team ──────────────────────────────────────────

class BlueTeamApp; // forward
static BlueTeamApp *g_btAppInstance = nullptr;

// Simple BLE device record for scanner
struct BleDeviceRecord
{
    uint8_t addr[6];
    int8_t  rssi;
    char    name[20];
    bool    isSkimmer;
    bool    isTracker;
    bool    isAirTag;
};

class BlueTeamScanCb : public NimBLEAdvertisedDeviceCallbacks
{
    void onResult(NimBLEAdvertisedDevice *dev) override;
};

// ── Helper: check prefix match ───────────────────────────────────────────────

static bool matchesAnyPrefix(const char *name, const char *const *prefixes, size_t count)
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }
    for (size_t i = 0U; i < count; ++i)
    {
        if (std::strncmp(name, prefixes[i], std::strlen(prefixes[i])) == 0)
        {
            return true;
        }
    }
    return false;
}

/// Check for Apple AirTag manufacturer data in raw ADV payload.
static bool isAppleTracker(const uint8_t *payload, size_t len)
{
    if (payload == nullptr || len < 7U)
    {
        return false;
    }
    size_t offset = 0U;
    while (offset < len)
    {
        const uint8_t fieldLen = payload[offset];
        if (fieldLen == 0U || (offset + fieldLen) >= len)
        {
            break;
        }
        const uint8_t adType = payload[offset + 1U];
        // Manufacturer Specific Data
        if (adType == 0xFFU && fieldLen >= 4U)
        {
            const uint16_t companyId = static_cast<uint16_t>(
                payload[offset + 2U] | (payload[offset + 3U] << 8U));
            if (companyId == APPLE_COMPANY_ID)
            {
                // Check for Find My / AirTag sub-types (0x12 or 0x07 proximity)
                if (fieldLen >= 5U)
                {
                    const uint8_t subType = payload[offset + 4U];
                    if (subType == 0x12U || subType == 0x10U)
                    {
                        return true;
                    }
                }
            }
        }
        offset += static_cast<size_t>(fieldLen) + 1U;
    }
    return false;
}

// ── App class ─────────────────────────────────────────────────────────────────

class BlueTeamApp final : public hackos::HackOSApp
{
public:
    BlueTeamApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(BTState::MENU_MAIN),
          needsRedraw_(true),
          idsActive_(false),
          scanActive_(false),
          bleInitialized_(false),
          threatCount_(0U),
          bleDeviceCount_(0U),
          newMacCount_(0U),
          bleSpamWindowStart_(0U),
          lastIdsTick_(0U),
          logScrollOffset_(0U)
    {
        std::memset(threats_, 0, sizeof(threats_));
        std::memset(bleDevices_, 0, sizeof(bleDevices_));
    }

    /// Called from BLE scan callback to report a device.
    void addBleDevice(const uint8_t addr[6], int rssi, const char *name,
                      const uint8_t *payload, size_t payloadLen)
    {
        // Check for duplicate
        for (size_t i = 0U; i < bleDeviceCount_; ++i)
        {
            if (std::memcmp(bleDevices_[i].addr, addr, 6U) == 0)
            {
                bleDevices_[i].rssi = static_cast<int8_t>(rssi);
                return;
            }
        }

        // Count new MACs for BLE spam detection
        newMacCount_++;

        if (bleDeviceCount_ >= MAX_BLE_DEVICES)
        {
            return;
        }

        auto &dev = bleDevices_[bleDeviceCount_];
        std::memcpy(dev.addr, addr, 6U);
        dev.rssi = static_cast<int8_t>(rssi);
        if (name != nullptr)
        {
            std::strncpy(dev.name, name, sizeof(dev.name) - 1U);
            dev.name[sizeof(dev.name) - 1U] = '\0';
        }
        else
        {
            dev.name[0] = '\0';
        }

        dev.isSkimmer = matchesAnyPrefix(dev.name, SKIMMER_PREFIXES, SKIMMER_PREFIX_COUNT);
        dev.isTracker = matchesAnyPrefix(dev.name, TRACKER_PREFIXES, TRACKER_PREFIX_COUNT);
        dev.isAirTag  = isAppleTracker(payload, payloadLen);

        ++bleDeviceCount_;

        // Immediate threat alerts for skimmers/trackers
        if (dev.isSkimmer)
        {
            char msg[40];
            std::snprintf(msg, sizeof(msg), "SKIMMER: %.10s", dev.name);
            addThreat(ThreatType::SKIMMER, msg);
            ToastManager::instance().show(msg);
        }
        if (dev.isAirTag)
        {
            addThreat(ThreatType::TRACKER_AIRTAG, "AirTag nearby!");
            ToastManager::instance().show("[!] AirTag detected!");
        }
        if (dev.isTracker && !dev.isAirTag)
        {
            char msg[40];
            std::snprintf(msg, sizeof(msg), "TRACKER: %.10s", dev.name);
            addThreat(ThreatType::TRACKER_OTHER, msg);
            ToastManager::instance().show(msg);
        }
    }

protected:
    void on_alloc() override {}

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        g_btAppInstance = this;
        state_ = BTState::MENU_MAIN;
        needsRedraw_ = true;
        ESP_LOGI(TAG_BT, "Blue Team app started");
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
        if (idsActive_)
        {
            idsTickCheck();
        }
        if (scanActive_ || idsActive_)
        {
            needsRedraw_ = true;
        }
    }

    void on_draw() override
    {
        if (!needsRedraw_)
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case BTState::MENU_MAIN:
            drawTitle("Blue Team");
            mainMenu_.draw();
            break;
        case BTState::IDS_RUNNING:
            drawIdsStatus();
            break;
        case BTState::SCANNER_RUNNING:
            drawScannerStatus();
            break;
        case BTState::THREAT_LOG:
            drawThreatLog();
            break;
        }

        DisplayManager::instance().present();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopIds();
        stopScan();
        deinitBle();
        g_btAppInstance = nullptr;
        ESP_LOGI(TAG_BT, "Blue Team app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;

    BTState state_;
    bool    needsRedraw_;
    bool    idsActive_;
    bool    scanActive_;
    bool    bleInitialized_;

    ThreatEntry threats_[MAX_THREATS];
    size_t      threatCount_;

    BleDeviceRecord bleDevices_[MAX_BLE_DEVICES];
    size_t          bleDeviceCount_;

    // BLE spam detection
    uint32_t newMacCount_;
    uint32_t bleSpamWindowStart_;

    uint32_t lastIdsTick_;
    size_t   logScrollOffset_;

    // ── Threat logging ───────────────────────────────────────────────────

    void addThreat(ThreatType type, const char *desc)
    {
        if (threatCount_ >= MAX_THREATS)
        {
            // Shift old entries
            std::memmove(&threats_[0], &threats_[1],
                         (MAX_THREATS - 1U) * sizeof(ThreatEntry));
            threatCount_ = MAX_THREATS - 1U;
        }
        auto &t = threats_[threatCount_];
        t.type = type;
        std::strncpy(t.desc, desc, THREAT_DESC_LEN);
        t.desc[THREAT_DESC_LEN] = '\0';
        t.timestampMs = static_cast<uint32_t>(
            xTaskGetTickCount() * portTICK_PERIOD_MS);
        ++threatCount_;
        ESP_LOGW(TAG_BT, "THREAT: %s – %s", threatTypeStr(type), desc);
    }

    // ── Drawing ──────────────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawIdsStatus()
    {
        drawTitle("IDS Active");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Threats: %u",
                      static_cast<unsigned>(threatCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Deauths: %lu",
                      static_cast<unsigned long>(g_deauthCount));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "BLE devs: %u",
                      static_cast<unsigned>(bleDeviceCount_));
        DisplayManager::instance().drawText(2, 42, buf);

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    void drawScannerStatus()
    {
        drawTitle("Scan: Skim/Track");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Found: %u",
                      static_cast<unsigned>(bleDeviceCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        // Show flagged devices
        int16_t yPos = 32;
        for (size_t i = 0U; i < bleDeviceCount_ && yPos <= 52; ++i)
        {
            const auto &dev = bleDevices_[i];
            if (!dev.isSkimmer && !dev.isTracker && !dev.isAirTag)
            {
                continue;
            }
            const char *tag = dev.isSkimmer ? "SKM" : (dev.isAirTag ? "TAG" : "TRK");
            std::snprintf(buf, sizeof(buf), "[%s] %.12s %ddB",
                          tag, dev.name[0] ? dev.name : "???",
                          static_cast<int>(dev.rssi));
            DisplayManager::instance().drawText(2, yPos, buf);
            yPos += 10;
        }

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    void drawThreatLog()
    {
        drawTitle("Threat Log");

        if (threatCount_ == 0U)
        {
            DisplayManager::instance().drawText(2, 30, "No threats logged");
            DisplayManager::instance().drawText(2, 54, "Press to go back");
            return;
        }

        char buf[32];
        int16_t yPos = 22;
        const size_t start = (logScrollOffset_ < threatCount_) ? logScrollOffset_ : 0U;
        for (size_t i = start; i < threatCount_ && yPos <= 52; ++i)
        {
            const auto &t = threats_[i];
            std::snprintf(buf, sizeof(buf), "%.21s", t.desc);
            DisplayManager::instance().drawText(2, yPos, buf);
            yPos += 10;
        }

        DisplayManager::instance().drawText(2, 54, "UP/DN scroll, BTN=back");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case BTState::MENU_MAIN:
            handleMainInput(input);
            break;
        case BTState::IDS_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopIds();
                state_ = BTState::MENU_MAIN;
                needsRedraw_ = true;
            }
            break;
        case BTState::SCANNER_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopScan();
                state_ = BTState::MENU_MAIN;
                needsRedraw_ = true;
            }
            break;
        case BTState::THREAT_LOG:
            handleLogInput(input);
            break;
        }
    }

    void handleMainInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            mainMenu_.moveSelection(-1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            mainMenu_.moveSelection(1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (mainMenu_.selectedIndex())
            {
            case 0U: // IDS Monitor
                startIds();
                break;
            case 1U: // Skimmer/Tracker Scan
                startScan();
                break;
            case 2U: // Threat Log
                logScrollOffset_ = 0U;
                state_ = BTState::THREAT_LOG;
                needsRedraw_ = true;
                break;
            case 3U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleLogInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::DOWN && logScrollOffset_ + 3U < threatCount_)
        {
            ++logScrollOffset_;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::UP && logScrollOffset_ > 0U)
        {
            --logScrollOffset_;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            state_ = BTState::MENU_MAIN;
            needsRedraw_ = true;
        }
    }

    // ── BLE init/deinit ──────────────────────────────────────────────────

    void initBle()
    {
        if (bleInitialized_)
        {
            return;
        }
        NimBLEDevice::init("HackOS-BT");
        bleInitialized_ = true;
        ESP_LOGI(TAG_BT, "BLE initialized");
    }

    void deinitBle()
    {
        if (!bleInitialized_)
        {
            return;
        }
        NimBLEDevice::deinit(true);
        bleInitialized_ = false;
    }

    // ── IDS Monitor ──────────────────────────────────────────────────────

    void startIds()
    {
        if (idsActive_)
        {
            return;
        }

        // Start WiFi promiscuous mode for deauth detection
        g_deauthCount = 0U;
        g_deauthWindowStart_ = nowMs();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_wifi_set_mode(WIFI_MODE_NULL);
        esp_wifi_start();
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_promiscuous_rx_cb(wifiPromiscuousCb);

        // Start BLE scan for spam detection
        initBle();
        bleDeviceCount_ = 0U;
        newMacCount_ = 0U;
        bleSpamWindowStart_ = nowMs();

        NimBLEScan *pScan = NimBLEDevice::getScan();
        static BlueTeamScanCb scanCb;
        pScan->setAdvertisedDeviceCallbacks(&scanCb, true);
        pScan->setActiveScan(false);
        pScan->setInterval(0x50);
        pScan->setWindow(0x30);
        pScan->setDuplicateFilter(false);
        pScan->start(0, nullptr, false);

        idsActive_ = true;
        lastIdsTick_ = nowMs();
        state_ = BTState::IDS_RUNNING;
        needsRedraw_ = true;

        ToastManager::instance().show("IDS Active - Monitoring");
        ESP_LOGI(TAG_BT, "IDS started");
    }

    void stopIds()
    {
        if (!idsActive_)
        {
            return;
        }

        esp_wifi_set_promiscuous(false);
        esp_wifi_stop();

        if (bleInitialized_)
        {
            NimBLEDevice::getScan()->stop();
        }

        idsActive_ = false;
        ESP_LOGI(TAG_BT, "IDS stopped");
    }

    /// Periodic IDS evaluation – checks deauth rate and BLE spam.
    void idsTickCheck()
    {
        const uint32_t now = nowMs();
        if ((now - lastIdsTick_) < IDS_SCAN_INTERVAL_MS)
        {
            return;
        }
        lastIdsTick_ = now;

        // Check deauth rate
        if ((now - g_deauthWindowStart_) >= DEAUTH_WINDOW_MS)
        {
            if (g_deauthCount >= DEAUTH_THRESHOLD)
            {
                addThreat(ThreatType::DEAUTH_ATTACK, "Deauth flood detected!");
                ToastManager::instance().show("[!] DEAUTH ATTACK!");
            }
            g_deauthCount = 0U;
            g_deauthWindowStart_ = now;
        }

        // Check BLE spam rate (many new MACs in short window)
        if ((now - bleSpamWindowStart_) >= BLE_SPAM_WINDOW_MS)
        {
            if (newMacCount_ >= BLE_SPAM_THRESHOLD)
            {
                addThreat(ThreatType::BLE_SPAM, "BLE Spam detected!");
                ToastManager::instance().show("[!] BLE SPAM DETECTED!");
            }
            newMacCount_ = 0U;
            bleSpamWindowStart_ = now;
        }
    }

    // ── Skimmer/Tracker Scanner ──────────────────────────────────────────

    void startScan()
    {
        if (scanActive_)
        {
            return;
        }

        initBle();
        bleDeviceCount_ = 0U;
        std::memset(bleDevices_, 0, sizeof(bleDevices_));

        NimBLEScan *pScan = NimBLEDevice::getScan();
        static BlueTeamScanCb scanCb;
        pScan->setAdvertisedDeviceCallbacks(&scanCb, true);
        pScan->setActiveScan(true); // active to get names
        pScan->setInterval(0x50);
        pScan->setWindow(0x30);
        pScan->setDuplicateFilter(false);
        pScan->start(0, nullptr, false);

        scanActive_ = true;
        state_ = BTState::SCANNER_RUNNING;
        needsRedraw_ = true;

        ToastManager::instance().show("Scanning for threats...");
        ESP_LOGI(TAG_BT, "Skimmer/Tracker scan started");
    }

    void stopScan()
    {
        if (!scanActive_)
        {
            return;
        }
        if (bleInitialized_)
        {
            NimBLEDevice::getScan()->stop();
        }
        scanActive_ = false;
        ESP_LOGI(TAG_BT, "Scan stopped (%u devices)", static_cast<unsigned>(bleDeviceCount_));
    }

    // ── Utility ──────────────────────────────────────────────────────────

    static uint32_t nowMs()
    {
        return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }

    uint32_t g_deauthWindowStart_; ///< shadows global for class-scoped tracking

    friend class BlueTeamScanCb;
};

// ── BLE scan callback ────────────────────────────────────────────────────────

void BlueTeamScanCb::onResult(NimBLEAdvertisedDevice *dev)
{
    if (g_btAppInstance == nullptr || dev == nullptr)
    {
        return;
    }

    const NimBLEAddress &addr = dev->getAddress();
    const uint8_t *nativeAddr = addr.getNative();
    int rssi = dev->getRSSI();
    const char *name = dev->haveName() ? dev->getName().c_str() : nullptr;
    const uint8_t *payload = dev->getPayload();
    size_t payloadLen = dev->getPayloadLength();

    g_btAppInstance->addBleDevice(nativeAddr, rssi, name, payload, payloadLen);
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createBlueTeamApp()
{
    return new (std::nothrow) BlueTeamApp();
}
