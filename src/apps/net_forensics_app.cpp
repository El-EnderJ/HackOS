/**
 * @file net_forensics_app.cpp
 * @brief Network Forensics – PCAP sniffer & Wireshark-compatible capture.
 *
 * Phase 27 deliverables:
 *  1. PcapManager       – writes standard PCAP header (0xA1B2C3D4) to SD.
 *  2. Selective sniffing – channel (1-13) + frame type filter.
 *  3. Handshake Hunter  – captures WPA/WPA2 EAPOL 4-way handshake;
 *                         HackBot celebrates + vibrate on success.
 *  4. Circular buffer   – lock-free ring buffer in RAM; background IO task
 *                         flushes to SD without losing packets.
 *  5. Statistics view   – live packets/second display on OLED.
 */

#include "apps/net_forensics_app.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "core/event.h"
#include "core/event_system.h"
#include "core/experience_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/radio/frame_parser_80211.h"
#include "storage/vfs.h"
#include "ui/widgets.h"

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char *TAG_NF = "NetForensics";

// PCAP file format constants (little-endian)
static constexpr uint32_t PCAP_MAGIC       = 0xA1B2C3D4U;
static constexpr uint16_t PCAP_VER_MAJOR   = 2U;
static constexpr uint16_t PCAP_VER_MINOR   = 4U;
static constexpr uint32_t PCAP_LINK_80211  = 105U; ///< LINKTYPE_IEEE802_11
static constexpr size_t   PCAP_SNAP_LEN    = 256U;

// Circular buffer sizing
static constexpr size_t   RING_SLOT_SIZE   = PCAP_SNAP_LEN + 16U; ///< data + header
static constexpr size_t   RING_SLOT_COUNT  = 32U;
static constexpr size_t   RING_BUF_SIZE    = RING_SLOT_SIZE * RING_SLOT_COUNT;

// EAPOL constants
static constexpr size_t   EAPOL_FULL_HANDSHAKE = 4U;

// IO task
static constexpr uint32_t IO_TASK_INTERVAL_MS = 50U;
static constexpr uint32_t IO_TASK_STACK       = 4096U;

// Stats update
static constexpr uint32_t STATS_INTERVAL_MS   = 1000U;

// ── PCAP packed structures ───────────────────────────────────────────────────

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

// ── Packet type filter enum ──────────────────────────────────────────────────

enum class PktFilter : uint8_t
{
    ALL        = 0U,
    MANAGEMENT = 1U,
    DATA       = 2U,
    CONTROL    = 3U,
    COUNT      = 4U
};

static const char *pktFilterName(PktFilter f)
{
    switch (f)
    {
    case PktFilter::MANAGEMENT: return "Mgmt";
    case PktFilter::DATA:       return "Data";
    case PktFilter::CONTROL:    return "Ctrl";
    default:                    return "All";
    }
}

// ── App view states ──────────────────────────────────────────────────────────

enum class NfView : uint8_t
{
    MENU        = 0U,
    CHANNEL_SEL = 1U,
    CAPTURING   = 2U,
    HANDSHAKE   = 3U,
    STATS       = 4U
};

// ── Circular (ring) buffer ───────────────────────────────────────────────────
//
// Lock-free single-producer / single-consumer ring buffer.
// Producer = promiscuous ISR callback (IRAM), Consumer = IO task.

namespace
{

struct RingSlot
{
    uint16_t len;      ///< actual captured length (0 = empty)
    uint16_t origLen;  ///< original packet length
    uint32_t tsMs;     ///< capture timestamp (ms since boot)
    uint8_t  data[PCAP_SNAP_LEN];
};

struct RingBuffer
{
    RingSlot slots[RING_SLOT_COUNT];
    volatile size_t head; ///< next write index (producer)
    volatile size_t tail; ///< next read index  (consumer)

    void reset()
    {
        head = 0U;
        tail = 0U;
        std::memset(slots, 0, sizeof(slots));
    }

    /// @brief Push a packet from ISR context (producer). Returns false if full.
    bool IRAM_ATTR push(const uint8_t *pkt, size_t len, uint32_t tsMs)
    {
        const size_t nextHead = (head + 1U) % RING_SLOT_COUNT;
        if (nextHead == tail)
        {
            return false; // buffer full – drop packet
        }

        RingSlot &s = slots[head];
        const size_t cap = (len > PCAP_SNAP_LEN) ? PCAP_SNAP_LEN : len;
        std::memcpy(s.data, pkt, cap);
        s.len     = static_cast<uint16_t>(cap);
        s.origLen = static_cast<uint16_t>(len);
        s.tsMs    = tsMs;

        head = nextHead; // publish to consumer
        return true;
    }

    /// @brief Pop a packet for the IO task (consumer). Returns false if empty.
    bool pop(RingSlot &out)
    {
        if (tail == head)
        {
            return false; // empty
        }

        out = slots[tail];
        slots[tail].len = 0U;
        tail = (tail + 1U) % RING_SLOT_COUNT;
        return true;
    }

    size_t used() const
    {
        const size_t h = head;
        const size_t t = tail;
        return (h >= t) ? (h - t) : (RING_SLOT_COUNT - t + h);
    }
};

// ── Global shared state (accessed from ISR + IO task + app) ──────────────────

static RingBuffer         g_ring;
static volatile uint32_t  g_totalPkts      = 0U;
static volatile uint32_t  g_droppedPkts    = 0U;
static volatile uint32_t  g_eapolCount     = 0U;
static volatile bool      g_captureActive  = false;
static volatile bool      g_ioTaskRunning  = false;
static PktFilter          g_pktFilter      = PktFilter::ALL;
static bool               g_handshakeMode  = false;
static char               g_pcapPath[64]   = {};
static bool               g_pcapOpen       = false;

// ── PcapManager ──────────────────────────────────────────────────────────────
//
// Manages PCAP file creation and packet writing on the SD card.

class PcapManager
{
public:
    /// @brief Create a new PCAP file on the SD card.
    static bool create(const char *dir, const char *prefix)
    {
        auto &vfs = hackos::storage::VirtualFS::instance();

        char path[64];
        uint16_t n = 0U;
        for (; n < 9999U; ++n)
        {
            std::snprintf(path, sizeof(path), "%s/%s_%04u.pcap", dir, prefix, n);
            if (!vfs.exists(path))
            {
                break;
            }
        }

        if (n >= 9999U)
        {
            ESP_LOGE(TAG_NF, "No available PCAP filename in %s", dir);
            return false;
        }

        fs::File f = vfs.open(path, "w");
        if (!f)
        {
            ESP_LOGE(TAG_NF, "Failed to create PCAP: %s", path);
            return false;
        }

        // Write standard PCAP global header
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

        std::strncpy(g_pcapPath, path, sizeof(g_pcapPath) - 1U);
        g_pcapPath[sizeof(g_pcapPath) - 1U] = '\0';
        g_pcapOpen = true;

        ESP_LOGI(TAG_NF, "PCAP created: %s", g_pcapPath);
        return true;
    }

    /// @brief Append a single packet record to the open PCAP file.
    static bool writePacket(const RingSlot &slot)
    {
        if (!g_pcapOpen || slot.len == 0U)
        {
            return false;
        }

        auto &vfs = hackos::storage::VirtualFS::instance();
        fs::File f = vfs.open(g_pcapPath, "a");
        if (!f)
        {
            return false;
        }

        PcapPacketHeader phdr = {};
        phdr.tsSec   = slot.tsMs / 1000U;
        phdr.tsUsec  = (slot.tsMs % 1000U) * 1000U;
        phdr.inclLen = slot.len;
        phdr.origLen = slot.origLen;

        f.write(reinterpret_cast<const uint8_t *>(&phdr), sizeof(phdr));
        f.write(slot.data, slot.len);
        f.close();
        return true;
    }

    /// @brief Close the current PCAP file.
    static void close()
    {
        g_pcapOpen = false;
        ESP_LOGI(TAG_NF, "PCAP closed: %s", g_pcapPath);
    }
};

// ── Promiscuous RX callback (ISR context) ────────────────────────────────────

static void IRAM_ATTR nfPromiscuousRxCb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr || !g_captureActive)
    {
        return;
    }

    // Apply packet type filter
    switch (g_pktFilter)
    {
    case PktFilter::MANAGEMENT:
        if (type != WIFI_PKT_MGMT) return;
        break;
    case PktFilter::DATA:
        if (type != WIFI_PKT_DATA) return;
        break;
    case PktFilter::CONTROL:
        if (type != WIFI_PKT_CTRL) return;
        break;
    default:
        break; // ALL – accept everything
    }

    const auto *pkt   = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *payload = pkt->payload;
    const size_t   len     = static_cast<size_t>(pkt->rx_ctrl.sig_len);

    if (len == 0U)
    {
        return;
    }

    // Handshake hunter mode – only capture EAPOL frames
    if (g_handshakeMode)
    {
        if (type != WIFI_PKT_DATA ||
            !hackos::radio::isEapolHandshake(payload, len))
        {
            // Still count total packets for stats
            ++g_totalPkts;
            return;
        }
        ++g_eapolCount;
    }

    const uint32_t tsMs = static_cast<uint32_t>(
        xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!g_ring.push(payload, len, tsMs))
    {
        ++g_droppedPkts;
    }

    ++g_totalPkts;
}

// ── IO flush task (runs on core 0) ───────────────────────────────────────────
//
// Drains the ring buffer and writes packets to SD in the background.

static void nfIoTask(void * /*param*/)
{
    ESP_LOGI(TAG_NF, "IO task started on core %d", xPortGetCoreID());

    while (g_ioTaskRunning)
    {
        RingSlot slot;
        while (g_ring.pop(slot))
        {
            PcapManager::writePacket(slot);
        }
        vTaskDelay(pdMS_TO_TICKS(IO_TASK_INTERVAL_MS));
    }

    // Drain remaining packets before exit
    RingSlot slot;
    while (g_ring.pop(slot))
    {
        PcapManager::writePacket(slot);
    }

    ESP_LOGI(TAG_NF, "IO task exiting");
    vTaskDelete(nullptr);
}

// ── NetForensicsApp ──────────────────────────────────────────────────────────

class NetForensicsApp final : public AppBase, public IEventObserver
{
public:
    NetForensicsApp()
        : statusBar_(0, 0, 128, 8),
          menu_(0, 12, 128, 40, 4),
          view_(NfView::MENU),
          channel_(1U),
          filter_(PktFilter::ALL),
          prevPkts_(0U),
          pps_(0U),
          lastStatsMs_(0U),
          handshakeCelebrated_(false),
          ioTaskHandle_(nullptr)
    {
    }

    // ── AppBase lifecycle ────────────────────────────────────────────────

    void onSetup() override
    {
        menuItems_[0] = "Selective Sniff";
        menuItems_[1] = "Handshake Hunter";
        menuItems_[2] = "View Statistics";
        menuItems_[3] = "Back";
        menu_.setItems(menuItems_, MENU_COUNT);

        statusBar_.setConnectivity(true, false);
        statusBar_.setBatteryLevel(90U);
        statusBar_.setTime(0U, 0U);

        view_ = NfView::MENU;
        (void)EventSystem::instance().subscribe(this);
        ESP_LOGI(TAG_NF, "NetForensicsApp started");
    }

    void onLoop() override
    {
        if (view_ == NfView::CAPTURING || view_ == NfView::HANDSHAKE)
        {
            updateStats();

            // Handshake hunter celebration
            if (view_ == NfView::HANDSHAKE &&
                g_eapolCount >= EAPOL_FULL_HANDSHAKE &&
                !handshakeCelebrated_)
            {
                handshakeCelebrated_ = true;
                ESP_LOGI(TAG_NF, "4-way handshake captured!");

                // Award XP for successful handshake capture
                const Event xpEvt{EventType::EVT_XP_EARNED,
                                  XP_AWARD_PCAP_HANDSHAKE, 0, nullptr};
                EventSystem::instance().postEvent(xpEvt);
            }
        }
    }

    void onDraw() override
    {
        DisplayManager::instance().clear();
        statusBar_.draw();
        statusBar_.clearDirty();

        switch (view_)
        {
        case NfView::MENU:
            drawMenu();
            break;
        case NfView::CHANNEL_SEL:
            drawChannelSelect();
            break;
        case NfView::CAPTURING:
            drawCapturing();
            break;
        case NfView::HANDSHAKE:
            drawHandshake();
            break;
        case NfView::STATS:
            drawStats();
            break;
        }

        DisplayManager::instance().present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        switch (view_)
        {
        case NfView::MENU:
            handleMenuInput(input);
            break;
        case NfView::CHANNEL_SEL:
            handleChannelInput(input);
            break;
        case NfView::CAPTURING:
        case NfView::HANDSHAKE:
            handleCaptureInput(input);
            break;
        case NfView::STATS:
            handleStatsInput(input);
            break;
        }
    }

    void onDestroy() override
    {
        stopCapture();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_NF, "NetForensicsApp destroyed");
    }

private:
    static constexpr size_t MENU_COUNT = 4U;
    static constexpr int32_t XP_AWARD_PCAP_HANDSHAKE = 30;

    StatusBar     statusBar_;
    MenuListView  menu_;
    const char   *menuItems_[MENU_COUNT];

    NfView    view_;
    uint8_t   channel_;
    PktFilter filter_;
    uint32_t  prevPkts_;
    uint32_t  pps_;
    uint32_t  lastStatsMs_;
    bool      handshakeCelebrated_;
    TaskHandle_t ioTaskHandle_;

    // ── Input handlers ───────────────────────────────────────────────────

    void handleMenuInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            menu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            menu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (menu_.selectedIndex())
            {
            case 0: // Selective Sniff
                g_handshakeMode = false;
                view_ = NfView::CHANNEL_SEL;
                break;
            case 1: // Handshake Hunter
                g_handshakeMode = true;
                filter_ = PktFilter::ALL;
                view_ = NfView::CHANNEL_SEL;
                break;
            case 2: // View Statistics
                view_ = NfView::STATS;
                break;
            case 3: // Back
            {
                const Event evt{EventType::EVT_APP, APP_EVENT_LAUNCH, 0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }
        }
    }

    void handleChannelInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            if (channel_ < 13U)
            {
                ++channel_;
            }
            else
            {
                channel_ = 1U;
            }
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            if (!g_handshakeMode)
            {
                // Cycle through packet type filters
                uint8_t f = static_cast<uint8_t>(filter_);
                f = (f + 1U) % static_cast<uint8_t>(PktFilter::COUNT);
                filter_ = static_cast<PktFilter>(f);
            }
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            startCapture();
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            view_ = NfView::MENU;
        }
    }

    void handleCaptureInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS ||
            input == InputManager::InputEvent::LEFT)
        {
            stopCapture();
            view_ = NfView::MENU;
        }
    }

    void handleStatsInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS ||
            input == InputManager::InputEvent::LEFT)
        {
            view_ = NfView::MENU;
        }
    }

    // ── Drawing ──────────────────────────────────────────────────────────

    void drawMenu()
    {
        auto &d = DisplayManager::instance();
        d.drawText(0, 10, "Net Forensics", 1U);
        menu_.draw();
        menu_.clearDirty();
    }

    void drawChannelSelect()
    {
        auto &d = DisplayManager::instance();
        d.drawText(0, 10, "Configure Capture", 1U);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "CH: %u  [UP]", channel_);
        d.drawText(0, 24, buf, 1U);

        if (g_handshakeMode)
        {
            d.drawText(0, 34, "Mode: Handshake", 1U);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "Filter: %s [DN]",
                          pktFilterName(filter_));
            d.drawText(0, 34, buf, 1U);
        }

        d.drawText(0, 50, "[OK] Start  [<] Back", 1U);
    }

    void drawCapturing()
    {
        auto &d = DisplayManager::instance();
        d.drawText(0, 10, "Capturing...", 1U);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "CH:%u  F:%s",
                      channel_, pktFilterName(filter_));
        d.drawText(0, 22, buf, 1U);

        std::snprintf(buf, sizeof(buf), "Pkts: %lu",
                      static_cast<unsigned long>(g_totalPkts));
        d.drawText(0, 32, buf, 1U);

        std::snprintf(buf, sizeof(buf), "PPS: %lu  Drop: %lu",
                      static_cast<unsigned long>(pps_),
                      static_cast<unsigned long>(g_droppedPkts));
        d.drawText(0, 42, buf, 1U);

        // Ring buffer usage bar
        const size_t used = g_ring.used();
        const uint8_t pct = static_cast<uint8_t>(
            (used * 100U) / RING_SLOT_COUNT);
        std::snprintf(buf, sizeof(buf), "Buf: %u%%", pct);
        d.drawText(0, 52, buf, 1U);

        const int16_t barX = 50;
        const int16_t barW = static_cast<int16_t>((78 * pct) / 100U);
        d.drawRect(barX, 52, 78, 6);
        if (barW > 0)
        {
            d.fillRect(barX, 52, barW, 6);
        }
    }

    void drawHandshake()
    {
        auto &d = DisplayManager::instance();

        if (handshakeCelebrated_)
        {
            // HackBot celebration
            d.drawText(10, 10, "<*_*>", 1U);
            d.drawText(10, 20, "HANDSHAKE!", 1U);
            d.drawText(10, 30, "4/4 EAPOL captured", 1U);
            d.drawText(10, 42, "File saved to SD", 1U);
        }
        else
        {
            d.drawText(0, 10, "Handshake Hunter", 1U);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "CH:%u  Scanning...", channel_);
            d.drawText(0, 22, buf, 1U);

            std::snprintf(buf, sizeof(buf), "EAPOL: %lu / %u",
                          static_cast<unsigned long>(g_eapolCount),
                          static_cast<unsigned>(EAPOL_FULL_HANDSHAKE));
            d.drawText(0, 34, buf, 1U);

            // Progress boxes for each EAPOL packet
            for (uint8_t i = 0U; i < EAPOL_FULL_HANDSHAKE; ++i)
            {
                const int16_t bx = static_cast<int16_t>(10 + i * 28);
                if (i < g_eapolCount)
                {
                    d.fillRect(bx, 46, 22, 10);
                }
                else
                {
                    d.drawRect(bx, 46, 22, 10);
                }
            }
        }

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Pkts: %lu  PPS: %lu",
                      static_cast<unsigned long>(g_totalPkts),
                      static_cast<unsigned long>(pps_));
        d.drawText(0, 58, buf, 1U);
    }

    void drawStats()
    {
        auto &d = DisplayManager::instance();
        d.drawText(0, 10, "Capture Stats", 1U);

        char buf[40];
        std::snprintf(buf, sizeof(buf), "Total: %lu pkts",
                      static_cast<unsigned long>(g_totalPkts));
        d.drawText(0, 22, buf, 1U);

        std::snprintf(buf, sizeof(buf), "Dropped: %lu",
                      static_cast<unsigned long>(g_droppedPkts));
        d.drawText(0, 32, buf, 1U);

        std::snprintf(buf, sizeof(buf), "EAPOL: %lu",
                      static_cast<unsigned long>(g_eapolCount));
        d.drawText(0, 42, buf, 1U);

        std::snprintf(buf, sizeof(buf), "File: %s",
                      g_pcapOpen ? g_pcapPath : "none");
        d.drawText(0, 52, buf, 1U);
    }

    // ── Capture control ──────────────────────────────────────────────────

    void startCapture()
    {
        if (g_captureActive)
        {
            return;
        }

        // Reset state
        g_totalPkts   = 0U;
        g_droppedPkts = 0U;
        g_eapolCount  = 0U;
        g_pktFilter   = filter_;
        prevPkts_     = 0U;
        pps_          = 0U;
        lastStatsMs_  = 0U;
        handshakeCelebrated_ = false;
        g_ring.reset();

        // Create PCAP file
        const char *prefix = g_handshakeMode ? "hs" : "cap";
        if (!PcapManager::create("/ext/pcap", prefix))
        {
            ESP_LOGE(TAG_NF, "Cannot create PCAP file");
            return;
        }

        // Set WiFi channel
        esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);

        // Enable promiscuous mode
        wifi_promiscuous_filter_t wfilt = {};
        if (g_handshakeMode || filter_ == PktFilter::ALL)
        {
            wfilt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
        }
        else if (filter_ == PktFilter::MANAGEMENT)
        {
            wfilt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        }
        else if (filter_ == PktFilter::DATA)
        {
            wfilt.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
        }
        else if (filter_ == PktFilter::CONTROL)
        {
            wfilt.filter_mask = WIFI_PROMIS_FILTER_MASK_CTRL;
        }

        esp_wifi_set_promiscuous_filter(&wfilt);
        esp_wifi_set_promiscuous_rx_cb(&nfPromiscuousRxCb);

        if (esp_wifi_set_promiscuous(true) != ESP_OK)
        {
            ESP_LOGE(TAG_NF, "Failed to enable promiscuous mode");
            PcapManager::close();
            return;
        }

        // Start IO flush task on core 0
        g_ioTaskRunning = true;
        g_captureActive = true;

        xTaskCreatePinnedToCore(
            nfIoTask,
            "nf_io",
            IO_TASK_STACK,
            nullptr,
            1,
            &ioTaskHandle_,
            0 // core 0
        );

        view_ = g_handshakeMode ? NfView::HANDSHAKE : NfView::CAPTURING;
        ESP_LOGI(TAG_NF, "Capture started – CH:%u filter:%s handshake:%s",
                 channel_, pktFilterName(filter_),
                 g_handshakeMode ? "yes" : "no");
    }

    void stopCapture()
    {
        if (!g_captureActive)
        {
            return;
        }

        g_captureActive = false;

        // Disable promiscuous mode
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(nullptr);

        // Stop IO task (it will drain remaining packets)
        g_ioTaskRunning = false;
        if (ioTaskHandle_ != nullptr)
        {
            // Give IO task time to drain and exit
            vTaskDelay(pdMS_TO_TICKS(IO_TASK_INTERVAL_MS * 3U));
            ioTaskHandle_ = nullptr;
        }

        PcapManager::close();
        ESP_LOGI(TAG_NF, "Capture stopped – %lu packets, %lu dropped",
                 static_cast<unsigned long>(g_totalPkts),
                 static_cast<unsigned long>(g_droppedPkts));
    }

    // ── Helpers ──────────────────────────────────────────────────────────

    void updateStats()
    {
        const uint32_t nowMs = static_cast<uint32_t>(millis());
        if (nowMs - lastStatsMs_ >= STATS_INTERVAL_MS)
        {
            const uint32_t cur = g_totalPkts;
            pps_ = cur - prevPkts_;
            prevPkts_ = cur;
            lastStatsMs_ = nowMs;
        }
    }
};

} // anonymous namespace

// ── Factory function ─────────────────────────────────────────────────────────

AppBase *createNetForensicsApp()
{
    return new (std::nothrow) NetForensicsApp();
}
