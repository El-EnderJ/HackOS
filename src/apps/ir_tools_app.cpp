#include "apps/ir_tools_app.h"

#include <IRutils.h>
#include <cstdio>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/ir_transceiver.h"
#include "ui/widgets.h"

static constexpr const char *TAG_IR_APP = "IRToolsApp";

namespace
{

enum class IRState : uint8_t
{
    MAIN_MENU,
    SNIFFER,
    CLONER,
};

static constexpr size_t IR_MENU_COUNT = 3U;
static const char *const IR_MENU_LABELS[IR_MENU_COUNT] = {"IR Sniffer", "IR Cloner", "Back"};

class IRToolsApp final : public AppBase, public IEventObserver
{
public:
    IRToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(IRState::MAIN_MENU),
          needsRedraw_(true),
          clonerSent_(false),
          protoName_{},
          codeHex_{}
    {
    }

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = IRState::MAIN_MENU;
        needsRedraw_ = true;
        ESP_LOGI(TAG_IR_APP, "setup");
    }

    void onLoop() override
    {
        if (state_ == IRState::SNIFFER)
        {
            pollSniffer();
        }
        else if (state_ == IRState::CLONER && !clonerSent_)
        {
            performClone();
        }
    }

    void onDraw() override
    {
        if (!needsRedraw_ && !statusBar_.isDirty() && !mainMenu_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();

        switch (state_)
        {
        case IRState::MAIN_MENU:
            drawTitle("IR Tools");
            mainMenu_.draw();
            break;
        case IRState::SNIFFER:
            drawTitle("IR Sniffer");
            drawSnifferView();
            break;
        case IRState::CLONER:
            drawTitle("IR Cloner");
            drawClonerView();
            break;
        }

        DisplayManager::instance().present();
        statusBar_.clearDirty();
        mainMenu_.clearDirty();
        needsRedraw_ = false;
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }
        handleInput(static_cast<InputManager::InputEvent>(event->arg0));
    }

    void onDestroy() override
    {
        IRTransceiver::instance().deinit();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_IR_APP, "destroyed");
    }

private:
    StatusBar statusBar_;
    MenuListView mainMenu_;
    IRState state_;
    bool needsRedraw_;
    bool clonerSent_;
    char protoName_[16];
    char codeHex_[20];

    void transitionTo(IRState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawSnifferView()
    {
        if (IRTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 24, protoName_);
            DisplayManager::instance().drawText(2, 36, codeHex_);
            char bits[16];
            std::snprintf(bits, sizeof(bits), "Bits: %u",
                          static_cast<unsigned>(IRTransceiver::instance().lastBits()));
            DisplayManager::instance().drawText(2, 48, bits);
        }
        else
        {
            DisplayManager::instance().drawText(2, 30, "Waiting for IR...");
        }
    }

    void drawClonerView()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 28, "No code captured.");
            DisplayManager::instance().drawText(2, 40, "Use Sniffer first");
        }
        else
        {
            DisplayManager::instance().drawText(2, 28, "Sending:");
            DisplayManager::instance().drawText(2, 40, codeHex_);
        }
    }

    void pollSniffer()
    {
        uint64_t value = 0U;
        decode_type_t proto = decode_type_t::UNKNOWN;
        uint16_t bits = 0U;

        if (IRTransceiver::instance().decode(value, proto, bits))
        {
            // Build protocol name string
            const String &protoStr = typeToString(proto, false);
            std::snprintf(protoName_, sizeof(protoName_), "%.15s", protoStr.c_str());

            // Build hex value string
            std::snprintf(codeHex_, sizeof(codeHex_), "0x%08llX",
                          static_cast<unsigned long long>(value));

            needsRedraw_ = true;
            ESP_LOGI(TAG_IR_APP, "sniffed: %s 0x%llX %ubits",
                     protoName_,
                     static_cast<unsigned long long>(value),
                     static_cast<unsigned>(bits));
        }
    }

    void performClone()
    {
        if (!IRTransceiver::instance().hasLastCode())
        {
            return;
        }

        IRTransceiver::instance().send(
            IRTransceiver::instance().lastValue(),
            IRTransceiver::instance().lastProtocol(),
            IRTransceiver::instance().lastBits());

        ESP_LOGI(TAG_IR_APP, "cloned code sent");
        clonerSent_ = true;
        // Stay in CLONER state so user can see the result; press back to return to menu
        needsRedraw_ = true;
    }

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case IRState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case IRState::SNIFFER:
        case IRState::CLONER:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                IRTransceiver::instance().deinit();
                transitionTo(IRState::MAIN_MENU);
                mainMenu_.setItems(IR_MENU_LABELS, IR_MENU_COUNT);
            }
            break;
        }
    }

    void handleMainMenu(InputManager::InputEvent input)
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
            const size_t sel = mainMenu_.selectedIndex();
            if (sel == 0U) // Sniffer
            {
                IRTransceiver::instance().initReceive();
                transitionTo(IRState::SNIFFER);
            }
            else if (sel == 1U) // Cloner
            {
                IRTransceiver::instance().initTransmit();
                clonerSent_ = false;
                transitionTo(IRState::CLONER);
            }
            else // Back
            {
                const Event evt{EventType::EVT_SYSTEM, SYSTEM_EVENT_BACK, 0, nullptr};
                EventSystem::instance().postEvent(evt);
            }
        }
    }
};

} // namespace

AppBase *createIRToolsApp()
{
    return new (std::nothrow) IRToolsApp();
}
