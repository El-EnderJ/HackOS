/**
 * @file ghostnet_app.cpp
 * @brief GhostNet App – ESP-NOW mesh radar, chat, sync, and remote
 *        execution interface for HackOS.
 *
 * Implements:
 *  - **Radar View**: Shows a live radar display of nearby GhostNet peers
 *    with RSSI-based distance visualisation and node count.
 *  - **Chat**: Send and receive free-form text messages between peers.
 *  - **Sync**: Broadcast captured WiFi/NFC/IR/RF data to all peers.
 *  - **Remote Exec**: Issue BLE spam or WiFi deauth commands to all
 *    peers (Master mode), or display incoming commands (Node mode).
 *
 * Uses the AppBase lifecycle so all work runs cooperatively inside the
 * Core_Task loop.
 *
 * @warning **Legal notice**: Coordinated wireless attacks against
 * networks or devices you do not own or have explicit written
 * authorisation to test is illegal in most jurisdictions.
 */

#include "apps/ghostnet_app.h"

#include <cstdio>
#include <cstring>
#include <new>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include "core/event.h"
#include "core/event_system.h"
#include "core/ghostnet_manager.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/widgets.h"

static constexpr const char *TAG_GNA = "GhostNetApp";

namespace
{

// ── Tunables ─────────────────────────────────────────────────────────────────

static constexpr size_t MENU_ITEM_COUNT      = 5U;
static constexpr size_t VISIBLE_ROWS         = 4U;
static constexpr size_t CHAT_VISIBLE_ROWS    = 4U;
static constexpr size_t CMD_MENU_ITEMS       = 4U;
static constexpr uint32_t RADAR_REFRESH_MS   = 500U;
static constexpr size_t LABEL_BUF_LEN        = 32U;

// ── App states ───────────────────────────────────────────────────────────────

enum class GhostState : uint8_t
{
    MAIN_MENU,
    RADAR,
    CHAT_VIEW,
    REMOTE_EXEC,
    SYNC_VIEW,
};

// ── Quick-send chat messages ─────────────────────────────────────────────────

static constexpr size_t QUICK_MSG_COUNT = 4U;
static const char *const QUICK_MSGS[QUICK_MSG_COUNT] = {
    "Hello!",
    "Ready",
    "Start",
    "Stop",
};

// ── GhostNetApp ──────────────────────────────────────────────────────────────

class GhostNetApp final : public AppBase, public IEventObserver
{
public:
    GhostNetApp()
        : statusBar_(0, 0, 128, 8)
        , menu_(0, 20, 128, 36, VISIBLE_ROWS)
        , state_(GhostState::MAIN_MENU)
        , needsRedraw_(true)
        , radarAngle_(0U)
        , lastRadarMs_(0U)
        , chatScrollOffset_(0U)
        , quickMsgIdx_(0U)
        , cmdMenuIdx_(0U)
    {
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    void onSetup() override
    {
        (void)EventSystem::instance().subscribe(this);

        // Initialise GhostNet if not already active.
        auto &gn = hackos::core::GhostNetManager::instance();
        if (!gn.isActive())
        {
            if (!gn.init())
            {
                ESP_LOGE(TAG_GNA, "GhostNet init failed");
            }
        }

        static const char *const mainItems[MENU_ITEM_COUNT] = {
            "Radar", "Chat", "Remote Exec", "Sync Data", "Back",
        };
        menu_.setItems(mainItems, MENU_ITEM_COUNT);

        statusBar_.setBatteryLevel(100U);
        state_ = GhostState::MAIN_MENU;
        needsRedraw_ = true;
    }

    void onLoop() override
    {
        // Tick the GhostNet manager.
        hackos::core::GhostNetManager::instance().tick();

        // Refresh radar animation.
        if (state_ == GhostState::RADAR)
        {
            const uint32_t now = millis();
            if ((now - lastRadarMs_) >= RADAR_REFRESH_MS)
            {
                radarAngle_ = (radarAngle_ + 30U) % 360U;
                lastRadarMs_ = now;
                needsRedraw_ = true;
            }
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !menu_.isDirty())
        {
            return;
        }

        auto &disp = DisplayManager::instance();
        disp.clear();
        statusBar_.draw();

        switch (state_)
        {
        case GhostState::MAIN_MENU:
            drawMainMenu(disp);
            break;
        case GhostState::RADAR:
            drawRadar(disp);
            break;
        case GhostState::CHAT_VIEW:
            drawChat(disp);
            break;
        case GhostState::REMOTE_EXEC:
            drawRemoteExec(disp);
            break;
        case GhostState::SYNC_VIEW:
            drawSyncView(disp);
            break;
        }

        disp.present();
        statusBar_.clearDirty();
        menu_.clearDirty();
        needsRedraw_ = false;
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr)
        {
            return;
        }

        // Handle GhostNet events (chat received, etc.).
        if (event->type == EventType::EVT_GHOSTNET)
        {
            needsRedraw_ = true;
            return;
        }

        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        switch (state_)
        {
        case GhostState::MAIN_MENU:
            handleMainMenuInput(input);
            break;
        case GhostState::RADAR:
            handleRadarInput(input);
            break;
        case GhostState::CHAT_VIEW:
            handleChatInput(input);
            break;
        case GhostState::REMOTE_EXEC:
            handleRemoteExecInput(input);
            break;
        case GhostState::SYNC_VIEW:
            handleSyncInput(input);
            break;
        }
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
        // Leave GhostNet running in background so it can receive messages.
    }

private:
    // ── Draw helpers ─────────────────────────────────────────────────────

    void drawMainMenu(DisplayManager &disp)
    {
        char title[32];
        const auto &gn = hackos::core::GhostNetManager::instance();
        std::snprintf(title, sizeof(title), "GhostNet [%s]", gn.nodeName());
        disp.drawText(2, 10, title);
        disp.drawLine(0, 18, 127, 18);
        menu_.draw();
    }

    void drawRadar(DisplayManager &disp)
    {
        const auto &gn = hackos::core::GhostNetManager::instance();
        const size_t peers = gn.peerCount();

        // Title bar.
        char hdr[32];
        std::snprintf(hdr, sizeof(hdr), "Radar  Nodes:%u", static_cast<unsigned>(peers));
        disp.drawText(0, 10, hdr);
        disp.drawLine(0, 18, 127, 18);

        // Radar centre at (32, 42), radius 20.
        static constexpr int16_t CX = 32;
        static constexpr int16_t CY = 42;
        static constexpr int16_t R1 = 8;
        static constexpr int16_t R2 = 15;
        static constexpr int16_t R3 = 20;

        // Draw concentric range circles.
        drawCircleBresenham(disp, CX, CY, R1);
        drawCircleBresenham(disp, CX, CY, R2);
        drawCircleBresenham(disp, CX, CY, R3);

        // Cross-hairs.
        disp.drawLine(CX - R3, CY, CX + R3, CY);
        disp.drawLine(CX, CY - R3, CX, CY + R3);

        // Plot peers as dots based on RSSI (stronger = closer to centre).
        size_t plotIdx = 0U;
        for (size_t i = 0U; i < hackos::core::GhostNetManager::MAX_PEERS; ++i)
        {
            const auto *p = gn.peer(i);
            if (p == nullptr || !p->active)
            {
                continue;
            }

            // Map RSSI (-30=close, -90=far) to distance from centre.
            int8_t clamped = p->rssi;
            if (clamped > -30) { clamped = -30; }
            if (clamped < -90) { clamped = -90; }
            const int16_t dist = static_cast<int16_t>(
                R3 * (static_cast<int>(clamped) + 30) / (-60));

            // Spread peers around the circle using their index.
            static constexpr int16_t ANGLES[] = {45, 135, 225, 315, 0, 90, 180, 270};
            const int16_t angle = ANGLES[plotIdx % 8U];
            // Approximate cos/sin with fixed values for 8 positions.
            static constexpr int16_t COS8[] = {7, -7, -7, 7, 10, 0, -10, 0};
            static constexpr int16_t SIN8[] = {-7, -7, 7, 7, 0, -10, 0, 10};
            const int16_t px = CX + (COS8[plotIdx % 8U] * dist) / 10;
            const int16_t py = CY + (SIN8[plotIdx % 8U] * dist) / 10;

            // Draw peer dot (3×3 filled).
            disp.fillRect(px - 1, py - 1, 3, 3);
            ++plotIdx;
        }

        // Peer list on the right side.
        int16_t listY = 20;
        for (size_t i = 0U; i < hackos::core::GhostNetManager::MAX_PEERS && listY < 62; ++i)
        {
            const auto *p = gn.peer(i);
            if (p == nullptr || !p->active)
            {
                continue;
            }
            char label[24];
            std::snprintf(label, sizeof(label), "%.8s %ddB",
                          p->name, static_cast<int>(p->rssi));
            disp.drawText(58, listY, label);
            listY += 10;
        }

        if (peers == 0U)
        {
            disp.drawText(58, 30, "Scanning...");
        }
    }

    /// @brief Bresenham circle (no Adafruit drawCircle dependency).
    static void drawCircleBresenham(DisplayManager &disp,
                                    int16_t cx, int16_t cy, int16_t r)
    {
        int16_t x = 0;
        int16_t y = r;
        int16_t d = 3 - 2 * r;
        while (x <= y)
        {
            disp.drawPixel(cx + x, cy + y);
            disp.drawPixel(cx - x, cy + y);
            disp.drawPixel(cx + x, cy - y);
            disp.drawPixel(cx - x, cy - y);
            disp.drawPixel(cx + y, cy + x);
            disp.drawPixel(cx - y, cy + x);
            disp.drawPixel(cx + y, cy - x);
            disp.drawPixel(cx - y, cy - x);
            if (d < 0)
            {
                d += 4 * x + 6;
            }
            else
            {
                d += 4 * (x - y) + 10;
                --y;
            }
            ++x;
        }
    }

    void drawChat(DisplayManager &disp)
    {
        const auto &gn = hackos::core::GhostNetManager::instance();

        disp.drawText(0, 10, "GhostNet Chat");
        disp.drawLine(0, 18, 127, 18);

        // Show chat messages.
        const size_t total = gn.chatCount();
        if (total == 0U)
        {
            disp.drawText(2, 28, "No messages yet");
        }
        else
        {
            const size_t start = (total > CHAT_VISIBLE_ROWS + chatScrollOffset_)
                                     ? total - CHAT_VISIBLE_ROWS - chatScrollOffset_
                                     : 0U;
            int16_t y = 20;
            for (size_t i = start; i < total && y < 52; ++i)
            {
                const auto *msg = gn.chatAt(i);
                if (msg != nullptr)
                {
                    char line[40];
                    std::snprintf(line, sizeof(line), "%.6s:%.20s",
                                  msg->sender, msg->text);
                    disp.drawText(0, y, line);
                    y += 8;
                }
            }
        }

        // Quick-send bar at bottom.
        char sendBar[32];
        std::snprintf(sendBar, sizeof(sendBar), "> %s", QUICK_MSGS[quickMsgIdx_]);
        disp.drawText(0, 56, sendBar);
    }

    void drawRemoteExec(DisplayManager &disp)
    {
        const auto &gn = hackos::core::GhostNetManager::instance();

        disp.drawText(0, 10, "Remote Exec (Master)");
        disp.drawLine(0, 18, 127, 18);

        static const char *const cmdLabels[CMD_MENU_ITEMS] = {
            "BLE Spam All",
            "Deauth All",
            "Stop All",
            "Back",
        };

        // Highlight selected command.
        for (size_t i = 0U; i < CMD_MENU_ITEMS; ++i)
        {
            const int16_t y = 22 + static_cast<int16_t>(i) * 10;
            if (i == cmdMenuIdx_)
            {
                disp.fillRect(0, y - 1, 128, 9);
                disp.drawText(2, y, cmdLabels[i], 1U, 0U); // inverted
            }
            else
            {
                disp.drawText(2, y, cmdLabels[i]);
            }
        }

        // Show last received command (node mode indicator).
        const auto lastCmd = gn.lastReceivedCmd();
        if (lastCmd != hackos::core::GhostCmd::NONE)
        {
            char cmdInfo[28];
            std::snprintf(cmdInfo, sizeof(cmdInfo), "RX Cmd: 0x%02X",
                          static_cast<unsigned>(lastCmd));
            disp.drawText(0, 56, cmdInfo);
        }
    }

    void drawSyncView(DisplayManager &disp)
    {
        const auto &gn = hackos::core::GhostNetManager::instance();

        disp.drawText(0, 10, "Data Sync");
        disp.drawLine(0, 18, 127, 18);

        char info[32];
        std::snprintf(info, sizeof(info), "Peers: %u",
                      static_cast<unsigned>(gn.peerCount()));
        disp.drawText(2, 24, info);
        disp.drawText(2, 34, "PRESS to broadcast");
        disp.drawText(2, 44, "captures to peers");
    }

    // ── Input handlers ───────────────────────────────────────────────────

    void handleMainMenuInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            menu_.moveSelection(-1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            menu_.moveSelection(1);
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            const size_t sel = menu_.selectedIndex();
            switch (sel)
            {
            case 0U: // Radar
                state_ = GhostState::RADAR;
                lastRadarMs_ = millis();
                needsRedraw_ = true;
                break;
            case 1U: // Chat
                state_ = GhostState::CHAT_VIEW;
                chatScrollOffset_ = 0U;
                quickMsgIdx_ = 0U;
                needsRedraw_ = true;
                break;
            case 2U: // Remote Exec
                state_ = GhostState::REMOTE_EXEC;
                cmdMenuIdx_ = 0U;
                needsRedraw_ = true;
                break;
            case 3U: // Sync Data
                state_ = GhostState::SYNC_VIEW;
                needsRedraw_ = true;
                break;
            case 4U: // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                                0, nullptr};
                EventSystem::instance().postEvent(evt);
                break;
            }
            default:
                break;
            }

            // Award XP for using GhostNet.
            if (sel < 4U)
            {
                const Event xpEvt{EventType::EVT_XP_EARNED, XP_GHOSTNET_OP,
                                  0, nullptr};
                EventSystem::instance().postEvent(xpEvt);
            }
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK,
                            0, nullptr};
            EventSystem::instance().postEvent(evt);
        }
    }

    void handleRadarInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::BUTTON_PRESS ||
            input == InputManager::InputEvent::LEFT)
        {
            state_ = GhostState::MAIN_MENU;
            needsRedraw_ = true;
        }
    }

    void handleChatInput(InputManager::InputEvent input)
    {
        auto &gn = hackos::core::GhostNetManager::instance();

        if (input == InputManager::InputEvent::LEFT)
        {
            state_ = GhostState::MAIN_MENU;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::UP)
        {
            // Scroll chat up.
            if (chatScrollOffset_ < gn.chatCount())
            {
                ++chatScrollOffset_;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            // Scroll chat down or cycle quick message.
            if (chatScrollOffset_ > 0U)
            {
                --chatScrollOffset_;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::RIGHT)
        {
            // Cycle quick-send message.
            quickMsgIdx_ = (quickMsgIdx_ + 1U) % QUICK_MSG_COUNT;
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Send selected quick message.
            (void)gn.sendChat(QUICK_MSGS[quickMsgIdx_]);
            chatScrollOffset_ = 0U;
            needsRedraw_ = true;
        }
    }

    void handleRemoteExecInput(InputManager::InputEvent input)
    {
        auto &gn = hackos::core::GhostNetManager::instance();

        if (input == InputManager::InputEvent::UP)
        {
            if (cmdMenuIdx_ > 0U)
            {
                --cmdMenuIdx_;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            if (cmdMenuIdx_ < CMD_MENU_ITEMS - 1U)
            {
                ++cmdMenuIdx_;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            switch (cmdMenuIdx_)
            {
            case 0U: // BLE Spam All
                (void)gn.sendCommand(hackos::core::GhostCmd::BLE_SPAM);
                ESP_LOGI(TAG_GNA, "Sent BLE_SPAM command to all peers");
                break;
            case 1U: // Deauth All
                (void)gn.sendCommand(hackos::core::GhostCmd::WIFI_DEAUTH);
                ESP_LOGI(TAG_GNA, "Sent WIFI_DEAUTH command to all peers");
                break;
            case 2U: // Stop All
                (void)gn.sendCommand(hackos::core::GhostCmd::STOP);
                ESP_LOGI(TAG_GNA, "Sent STOP command to all peers");
                break;
            case 3U: // Back
                state_ = GhostState::MAIN_MENU;
                break;
            default:
                break;
            }
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            state_ = GhostState::MAIN_MENU;
            needsRedraw_ = true;
        }
    }

    void handleSyncInput(InputManager::InputEvent input)
    {
        auto &gn = hackos::core::GhostNetManager::instance();

        if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            // Broadcast a small sync payload as a demonstration.
            const char *syncType = "PING";
            const uint8_t pingData[] = {0x01};
            (void)gn.syncData(syncType, pingData, sizeof(pingData));
            ESP_LOGI(TAG_GNA, "Sync broadcast sent");
            needsRedraw_ = true;
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            state_ = GhostState::MAIN_MENU;
            needsRedraw_ = true;
        }
    }

    // ── Members ──────────────────────────────────────────────────────────

    StatusBar    statusBar_;
    MenuListView menu_;
    GhostState   state_;
    bool         needsRedraw_;

    // Radar.
    uint16_t radarAngle_;
    uint32_t lastRadarMs_;

    // Chat.
    size_t chatScrollOffset_;
    size_t quickMsgIdx_;

    // Remote exec.
    size_t cmdMenuIdx_;
};

} // namespace

AppBase *createGhostNetApp()
{
    return new (std::nothrow) GhostNetApp();
}
