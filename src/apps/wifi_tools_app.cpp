#include "apps/wifi_tools_app.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/storage.h"
#include "hardware/wireless.h"
#include "ui/widgets.h"

static constexpr const char *TAG_WIFI = "WiFiToolsApp";

namespace
{

// ── Internal state machine ────────────────────────────────────────────────────

enum class WiFiState : uint8_t
{
    MENU_PRINCIPAL,
    SCANNING,
    AP_LIST,
    ATTACK_MENU,
    DEAUTHING,
};

// ── Static menu labels ────────────────────────────────────────────────────────

static constexpr size_t MAIN_MENU_COUNT = 2U;
static const char *const MAIN_MENU_LABELS[MAIN_MENU_COUNT] = {"Scan Networks", "Back"};

static constexpr size_t ATTACK_MENU_COUNT = 4U;
static const char *const ATTACK_MENU_LABELS[ATTACK_MENU_COUNT] = {"Deauth", "Evil Twin", "Info", "Save AP"};

// ── App class ─────────────────────────────────────────────────────────────────

class WiFiToolsApp final : public AppBase, public IEventObserver
{
public:
    WiFiToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          apMenu_(0, 20, 128, 36, 3),
          attackMenu_(0, 20, 128, 30, 3),
          progressBar_(0, 54, 128, 10),
          state_(WiFiState::MENU_PRINCIPAL),
          needsRedraw_(true),
          scanAnimTick_(0U),
          apCount_(0U),
          selectedApIndex_(0U),
          deauthCount_(0U),
          apLabels_{},
          apPtrs_{}
    {
    }

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(MAIN_MENU_LABELS, MAIN_MENU_COUNT);
        attackMenu_.setItems(ATTACK_MENU_LABELS, ATTACK_MENU_COUNT);
        progressBar_.setProgress(0U);
        (void)EventSystem::instance().subscribe(this);
        (void)Wireless::instance().init();
        state_ = WiFiState::MENU_PRINCIPAL;
        needsRedraw_ = true;
        ESP_LOGI(TAG_WIFI, "setup");
    }

    void onLoop() override
    {
        switch (state_)
        {
        case WiFiState::SCANNING:
            animateScan();
            break;
        case WiFiState::DEAUTHING:
            performDeauthStep();
            break;
        default:
            break;
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !anyWidgetDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case WiFiState::MENU_PRINCIPAL:
            drawTitle("WiFi Tools");
            mainMenu_.draw();
            break;
        case WiFiState::SCANNING:
            drawTitle("Scanning...");
            progressBar_.draw();
            break;
        case WiFiState::AP_LIST:
            drawTitle("AP List");
            if (apCount_ > 0U)
            {
                apMenu_.draw();
            }
            else
            {
                DisplayManager::instance().drawText(4, 28, "No APs found");
            }
            break;
        case WiFiState::ATTACK_MENU:
            drawApHeader();
            attackMenu_.draw();
            break;
        case WiFiState::DEAUTHING:
            drawDeauthStatus();
            break;
        }

        DisplayManager::instance().present();
        clearAllDirty();
        needsRedraw_ = false;
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr)
        {
            return;
        }

        if (event->type == EventType::EVT_WIFI_SCAN_DONE)
        {
            // Guard against double delivery (AppManager + direct subscription)
            if (state_ == WiFiState::SCANNING)
            {
                onScanDone(static_cast<uint8_t>(event->arg0));
            }
            return;
        }

        if (event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);
        handleInput(input);
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
        Wireless::instance().deinit();
        freeApLabels();
        ESP_LOGI(TAG_WIFI, "destroyed – WiFi deinit, labels freed");
    }

private:
    static constexpr size_t MAX_APS = Wireless::MAX_APS;
    static constexpr size_t AP_LABEL_LEN = 32U; // ssid(22) + space + sign+digits(4) + "dBm"(3) + NUL
    static constexpr uint8_t MAX_DEAUTH_BURSTS = 50U;
    static constexpr uint8_t DEAUTH_PER_BURST = 1U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    MenuListView apMenu_;
    MenuListView attackMenu_;
    ProgressBar progressBar_;

    WiFiState state_;
    bool needsRedraw_;
    uint32_t scanAnimTick_;
    uint8_t apCount_;
    size_t selectedApIndex_;
    uint8_t deauthCount_;

    char *apLabels_[MAX_APS];      ///< Heap-allocated display strings for each AP
    const char *apPtrs_[MAX_APS];  ///< Const-pointer view used by MenuListView

    // ── Dirty / redraw helpers ────────────────────────────────────────────────

    bool anyWidgetDirty() const
    {
        return statusBar_.isDirty() || mainMenu_.isDirty() || apMenu_.isDirty() ||
               attackMenu_.isDirty() || progressBar_.isDirty();
    }

    void clearAllDirty()
    {
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        apMenu_.clearDirty();
        attackMenu_.clearDirty();
        progressBar_.clearDirty();
    }

    void transitionTo(WiFiState next)
    {
        state_ = next;
        needsRedraw_ = true;
        ESP_LOGD(TAG_WIFI, "state -> %d", static_cast<int>(next));
    }

    // ── Drawing helpers ───────────────────────────────────────────────────────

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawApHeader()
    {
        const Wireless::ApRecord *aps = Wireless::instance().aps();
        if (aps != nullptr && selectedApIndex_ < apCount_)
        {
            char buf[21];
            const char *ssid = aps[selectedApIndex_].ssid;
            std::snprintf(buf, sizeof(buf), "%.20s", ssid[0] != '\0' ? ssid : "[hidden]");
            DisplayManager::instance().drawText(2, 10, buf);
            DisplayManager::instance().drawLine(0, 18, 127, 18);
        }
    }

    void drawDeauthStatus()
    {
        const Wireless::ApRecord *aps = Wireless::instance().aps();
        if (aps == nullptr || selectedApIndex_ >= apCount_)
        {
            return;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Deauth: %.14s", aps[selectedApIndex_].ssid);
        DisplayManager::instance().drawText(2, 12, buf);
        std::snprintf(buf, sizeof(buf), "Frames: %u", static_cast<unsigned>(deauthCount_));
        DisplayManager::instance().drawText(2, 24, buf);
        DisplayManager::instance().drawText(2, 36, "Press to stop");
    }

    // ── Input dispatching ─────────────────────────────────────────────────────

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case WiFiState::MENU_PRINCIPAL:
            handleMainMenuInput(input);
            break;
        case WiFiState::SCANNING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                transitionTo(WiFiState::MENU_PRINCIPAL);
            }
            break;
        case WiFiState::AP_LIST:
            handleApListInput(input);
            break;
        case WiFiState::ATTACK_MENU:
            handleAttackMenuInput(input);
            break;
        case WiFiState::DEAUTHING:
            if (input == InputManager::InputEvent::BUTTON_PRESS)
            {
                transitionTo(WiFiState::ATTACK_MENU);
            }
            break;
        }
    }

    void handleMainMenuInput(InputManager::InputEvent input)
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
            if (mainMenu_.selectedIndex() == 0U)
            {
                startScan();
            }
            else
            {
                // "Back" – exit the app via the global back event
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }

    void handleApListInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            apMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            apMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            selectedApIndex_ = apMenu_.selectedIndex();
            attackMenu_.setItems(ATTACK_MENU_LABELS, ATTACK_MENU_COUNT);
            transitionTo(WiFiState::ATTACK_MENU);
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(WiFiState::MENU_PRINCIPAL);
        }
    }

    void handleAttackMenuInput(InputManager::InputEvent input)
    {
        if (input == InputManager::InputEvent::UP)
        {
            attackMenu_.moveSelection(-1);
        }
        else if (input == InputManager::InputEvent::DOWN)
        {
            attackMenu_.moveSelection(1);
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            executeAttack(attackMenu_.selectedIndex());
        }
        else if (input == InputManager::InputEvent::LEFT)
        {
            transitionTo(WiFiState::AP_LIST);
        }
    }

    // ── Scan ──────────────────────────────────────────────────────────────────

    void startScan()
    {
        freeApLabels();
        apCount_ = 0U;
        if (Wireless::instance().startScan())
        {
            scanAnimTick_ = 0U;
            progressBar_.setProgress(0U);
            transitionTo(WiFiState::SCANNING);
        }
    }

    void animateScan()
    {
        ++scanAnimTick_;
        // Sweep 0→100 repeatedly to create a running progress animation
        const uint8_t progress = static_cast<uint8_t>(scanAnimTick_ % 101U);
        progressBar_.setProgress(progress);
    }

    void onScanDone(uint8_t count)
    {
        buildApLabels(count);
        apMenu_.setItems(apPtrs_, apCount_);
        statusBar_.setConnectivity(false, true);
        transitionTo(WiFiState::AP_LIST);
        ESP_LOGI(TAG_WIFI, "Scan done: %u AP(s)", static_cast<unsigned>(apCount_));
    }

    // ── AP label management (heap) ────────────────────────────────────────────

    void buildApLabels(uint8_t count)
    {
        freeApLabels(); // clears strings and nulls pointers

        const Wireless::ApRecord *aps = Wireless::instance().aps();
        if (aps == nullptr || count == 0U)
        {
            return;
        }

        const uint8_t actual = count < static_cast<uint8_t>(MAX_APS)
                                   ? count
                                   : static_cast<uint8_t>(MAX_APS);
        for (uint8_t i = 0U; i < actual; ++i)
        {
            apLabels_[i] = new (std::nothrow) char[AP_LABEL_LEN];
            if (apLabels_[i] != nullptr)
            {
                const char *ssid = aps[i].ssid[0] != '\0' ? aps[i].ssid : "[hidden]";
                std::snprintf(apLabels_[i], AP_LABEL_LEN, "%.22s %ddBm",
                              ssid, static_cast<int>(aps[i].rssi));
            }
            apPtrs_[i] = apLabels_[i];
        }
        apCount_ = actual;
    }

    void freeApLabels()
    {
        for (size_t i = 0U; i < MAX_APS; ++i)
        {
            delete[] apLabels_[i];
            apLabels_[i] = nullptr;
            apPtrs_[i] = nullptr;
        }
    }

    // ── Attack execution ──────────────────────────────────────────────────────

    void executeAttack(size_t option)
    {
        switch (option)
        {
        case 0U: // Deauth
            deauthCount_ = 0U;
            transitionTo(WiFiState::DEAUTHING);
            break;
        case 1U: // Evil Twin – mock only
            ESP_LOGI(TAG_WIFI, "Evil Twin: mock – not implemented");
            transitionTo(WiFiState::ATTACK_MENU);
            break;
        case 2U: // Info
        {
            const Wireless::ApRecord *aps = Wireless::instance().aps();
            if (aps != nullptr && selectedApIndex_ < apCount_)
            {
                const Wireless::ApRecord &ap = aps[selectedApIndex_];
                ESP_LOGI(TAG_WIFI, "AP Info: SSID=%s BSSID=%02X:%02X:%02X:%02X:%02X:%02X "
                                   "RSSI=%d CH=%u Auth=%u",
                         ap.ssid,
                         ap.bssid[0], ap.bssid[1], ap.bssid[2],
                         ap.bssid[3], ap.bssid[4], ap.bssid[5],
                         static_cast<int>(ap.rssi),
                         static_cast<unsigned>(ap.channel),
                         static_cast<unsigned>(ap.authmode));
            }
            transitionTo(WiFiState::ATTACK_MENU);
            break;
        }
        case 3U: // Save AP
            saveApToSd();
            transitionTo(WiFiState::ATTACK_MENU);
            break;
        default:
            break;
        }
    }

    void performDeauthStep()
    {
        if (deauthCount_ >= MAX_DEAUTH_BURSTS)
        {
            transitionTo(WiFiState::ATTACK_MENU);
            return;
        }

        const Wireless::ApRecord *aps = Wireless::instance().aps();
        if (aps == nullptr || selectedApIndex_ >= apCount_)
        {
            transitionTo(WiFiState::ATTACK_MENU);
            return;
        }

        const Wireless::ApRecord &ap = aps[selectedApIndex_];
        if (Wireless::instance().sendDeauth(ap.bssid, ap.channel, DEAUTH_PER_BURST))
        {
            ++deauthCount_;
            needsRedraw_ = true;
        }
    }

    // ── Save AP to SD ─────────────────────────────────────────────────────────

    void saveApToSd()
    {
        const Wireless::ApRecord *aps = Wireless::instance().aps();
        if (aps == nullptr || selectedApIndex_ >= apCount_)
        {
            ESP_LOGW(TAG_WIFI, "saveApToSd: no AP selected");
            return;
        }
        if (!StorageManager::instance().isMounted())
        {
            ESP_LOGW(TAG_WIFI, "saveApToSd: SD not mounted");
            return;
        }

        const Wireless::ApRecord &ap = aps[selectedApIndex_];
        char buf[128];
        const int len = std::snprintf(buf, sizeof(buf),
            "SSID: %s\nBSSID: %02X:%02X:%02X:%02X:%02X:%02X\n"
            "RSSI: %d dBm\nChannel: %u\nAuth: %u\n",
            ap.ssid[0] != '\0' ? ap.ssid : "[hidden]",
            ap.bssid[0], ap.bssid[1], ap.bssid[2],
            ap.bssid[3], ap.bssid[4], ap.bssid[5],
            static_cast<int>(ap.rssi),
            static_cast<unsigned>(ap.channel),
            static_cast<unsigned>(ap.authmode));

        if (len > 0 && len < static_cast<int>(sizeof(buf)))
        {
            const bool ok = StorageManager::instance().appendChunk(
                "/captures/wifi_scan.txt",
                reinterpret_cast<const uint8_t *>(buf),
                static_cast<size_t>(len));
            ESP_LOGI(TAG_WIFI, "saveApToSd: %s", ok ? "OK" : "FAIL");
        }
    }
};

} // namespace

AppBase *createWifiToolsApp()
{
    return new (std::nothrow) WiFiToolsApp();
}
