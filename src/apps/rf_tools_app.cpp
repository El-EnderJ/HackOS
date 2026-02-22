#include "apps/rf_tools_app.h"

#include <cstdio>
#include <esp_log.h>
#include <new>

#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "hardware/rf_transceiver.h"
#include "ui/widgets.h"

static constexpr const char *TAG_RF_APP = "RFToolsApp";

namespace
{

enum class RFState : uint8_t
{
    MAIN_MENU,
    RECEIVING,
    TRANSMITTING,
};

static constexpr size_t RF_MENU_COUNT = 3U;
static const char *const RF_MENU_LABELS[RF_MENU_COUNT] = {"RF Receiver", "RF Transmitter", "Back"};

class RFToolsApp final : public AppBase, public IEventObserver
{
public:
    RFToolsApp()
        : statusBar_(0, 0, 128, 8),
          mainMenu_(0, 20, 128, 36, 3),
          state_(RFState::MAIN_MENU),
          needsRedraw_(true),
          txSent_(false),
          codeLine_{},
          bitLine_{}
    {
    }

    void onSetup() override
    {
        statusBar_.setConnectivity(false, false);
        statusBar_.setBatteryLevel(100U);
        statusBar_.setTime(0U, 0U);
        mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
        (void)EventSystem::instance().subscribe(this);
        state_ = RFState::MAIN_MENU;
        needsRedraw_ = true;
        ESP_LOGI(TAG_RF_APP, "setup");
    }

    void onLoop() override
    {
        if (state_ == RFState::RECEIVING)
        {
            pollReceiver();
        }
        else if (state_ == RFState::TRANSMITTING && !txSent_)
        {
            performTransmit();
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
        case RFState::MAIN_MENU:
            drawTitle("RF 433MHz");
            mainMenu_.draw();
            break;
        case RFState::RECEIVING:
            drawTitle("RF Receiver");
            drawReceiverView();
            break;
        case RFState::TRANSMITTING:
            drawTitle("RF Transmitter");
            drawTransmitterView();
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
        RFTransceiver::instance().deinit();
        EventSystem::instance().unsubscribe(this);
        ESP_LOGI(TAG_RF_APP, "destroyed");
    }

private:
    static constexpr size_t LINE_LEN = 24U;

    StatusBar statusBar_;
    MenuListView mainMenu_;
    RFState state_;
    bool needsRedraw_;
    bool txSent_;
    char codeLine_[LINE_LEN];
    char bitLine_[LINE_LEN];

    void transitionTo(RFState next)
    {
        state_ = next;
        needsRedraw_ = true;
    }

    void drawTitle(const char *title)
    {
        DisplayManager::instance().drawText(2, 10, title);
        DisplayManager::instance().drawLine(0, 18, 127, 18);
    }

    void drawReceiverView()
    {
        if (RFTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 24, codeLine_);
            DisplayManager::instance().drawText(2, 36, bitLine_);
            DisplayManager::instance().drawText(2, 50, "Press to exit");
        }
        else
        {
            DisplayManager::instance().drawText(2, 30, "Listening...");
        }
    }

    void drawTransmitterView()
    {
        if (!RFTransceiver::instance().hasLastCode())
        {
            DisplayManager::instance().drawText(2, 28, "No code captured.");
            DisplayManager::instance().drawText(2, 40, "Use Receiver first");
        }
        else
        {
            DisplayManager::instance().drawText(2, 28, "Sending:");
            DisplayManager::instance().drawText(2, 40, codeLine_);
        }
    }

    void pollReceiver()
    {
        unsigned long code = 0UL;
        unsigned int bits = 0U;
        unsigned int delay = 0U;
        if (RFTransceiver::instance().read(code, bits, delay))
        {
            std::snprintf(codeLine_, sizeof(codeLine_), "Code: %lu", code);
            std::snprintf(bitLine_, sizeof(bitLine_), "Bits:%u Dly:%u", bits, delay);
            needsRedraw_ = true;
            ESP_LOGI(TAG_RF_APP, "received: code=%lu bits=%u delay=%u",
                     code, static_cast<unsigned>(bits), static_cast<unsigned>(delay));
        }
    }

    void performTransmit()
    {
        if (!RFTransceiver::instance().hasLastCode())
        {
            return;
        }

        RFTransceiver::instance().send(
            RFTransceiver::instance().lastCode(),
            RFTransceiver::instance().lastBitLength());

        ESP_LOGI(TAG_RF_APP, "transmitted last code");
        txSent_ = true;
        // Stay in TRANSMITTING state so user can see the result and press back when done
        needsRedraw_ = true;
    }

    void handleInput(InputManager::InputEvent input)
    {
        switch (state_)
        {
        case RFState::MAIN_MENU:
            handleMainMenu(input);
            break;
        case RFState::RECEIVING:
        case RFState::TRANSMITTING:
            if (input == InputManager::InputEvent::BUTTON_PRESS ||
                input == InputManager::InputEvent::LEFT)
            {
                RFTransceiver::instance().deinit();
                transitionTo(RFState::MAIN_MENU);
                mainMenu_.setItems(RF_MENU_LABELS, RF_MENU_COUNT);
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
            if (sel == 0U) // Receiver
            {
                RFTransceiver::instance().initReceive();
                transitionTo(RFState::RECEIVING);
            }
            else if (sel == 1U) // Transmitter
            {
                RFTransceiver::instance().initTransmit();
                txSent_ = false;
                transitionTo(RFState::TRANSMITTING);
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

AppBase *createRFToolsApp()
{
    return new (std::nothrow) RFToolsApp();
}
