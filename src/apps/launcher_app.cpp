#include "apps/launcher_app.h"

#include "core/app_manager.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/widgets.h"

namespace
{
class LauncherApp final : public AppBase, public IEventObserver
{
public:
    LauncherApp()
        : statusBar_(0, 0, 128, 8),
          menu_(0, 12, 128, 40, 4),
          progressBar_(0, 54, 128, 10),
          dialog_(14, 16, 100, 30),
          appNames_{},
          appCount_(0U)
    {
    }

    void onSetup() override
    {
        appCount_ = AppManager::instance().appCount();
        for (size_t i = 0; i < appCount_ && i < MAX_APPS; ++i)
        {
            appNames_[i] = AppManager::instance().appNameAt(i);
        }

        menu_.setItems(appNames_, appCount_);
        statusBar_.setConnectivity(true, true);
        statusBar_.setBatteryLevel(85U);
        statusBar_.setTime(12U, 34U);
        dialog_.setText("HackOS", "Ready");
        (void)EventSystem::instance().subscribe(this);
    }

    void onLoop() override {}

    void onDraw() override
    {
        if (!statusBar_.isDirty() && !menu_.isDirty() && !progressBar_.isDirty() && !dialog_.isDirty())
        {
            return;
        }

        DisplayManager::instance().clear();
        statusBar_.draw();
        menu_.draw();
        progressBar_.draw();
        dialog_.draw();
        DisplayManager::instance().present();

        statusBar_.clearDirty();
        menu_.clearDirty();
        progressBar_.clearDirty();
        dialog_.clearDirty();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);
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
            dialog_.setVisible(!dialog_.isVisible());
        }

        updateProgressIndicator();
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
    }

private:
    static constexpr size_t MAX_APPS = 8U;
    StatusBar statusBar_;
    MenuListView menu_;
    ProgressBar progressBar_;
    DialogBox dialog_;
    const char *appNames_[MAX_APPS];
    size_t appCount_;

    void updateProgressIndicator()
    {
        if (menu_.itemCount() > 1U)
        {
            progressBar_.setProgress(static_cast<uint8_t>(
                (menu_.selectedIndex() * 100U + (menu_.itemCount() - 1U) / 2U) / (menu_.itemCount() - 1U)));
        }
        else
        {
            progressBar_.setProgress(0U);
        }
    }
};
} // namespace

AppBase *createLauncherApp()
{
    return new LauncherApp();
}
