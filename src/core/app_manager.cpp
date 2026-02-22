#include "core/app_manager.h"

#include <cstring>

#include "core/state_machine.h"
#include "hardware/input.h"

AppManager &AppManager::instance()
{
    static AppManager manager;
    return manager;
}

AppManager::AppManager()
    : apps_{},
      appCount_(0U),
      activeApp_(nullptr),
      lastDrawMs_(0U)
{
}

bool AppManager::init()
{
    return EventSystem::instance().subscribe(this);
}

bool AppManager::registerApp(const char *name, AppFactory factory)
{
    if (name == nullptr || factory == nullptr || appCount_ >= MAX_APPS)
    {
        return false;
    }

    apps_[appCount_] = {name, factory};
    ++appCount_;
    return true;
}

bool AppManager::launchApp(const char *name)
{
    if (name == nullptr)
    {
        return false;
    }

    for (size_t i = 0; i < appCount_; ++i)
    {
        if (std::strcmp(apps_[i].name, name) == 0)
        {
            destroyActiveApp();
            activeApp_ = apps_[i].factory();
            if (activeApp_ == nullptr)
            {
                return false;
            }
            activeApp_->onSetup();
            (void)StateMachine::instance().pushState(GlobalState::APP_RUNNING);
            lastDrawMs_ = 0U;
            return true;
        }
    }

    return false;
}

void AppManager::loop()
{
    static constexpr uint32_t FRAME_INTERVAL_MS = 33U;
    const InputManager::InputEvent input = InputManager::instance().readInput();
    if (input != InputManager::InputEvent::CENTER)
    {
        Event inputEvent{EventType::EVT_INPUT, static_cast<int32_t>(input), 0, nullptr};
        (void)EventSystem::instance().postEvent(inputEvent);
    }

    if (activeApp_ == nullptr)
    {
        return;
    }

    activeApp_->onLoop();
    const uint32_t now = millis();
    if ((now - lastDrawMs_) >= FRAME_INTERVAL_MS)
    {
        activeApp_->onDraw();
        lastDrawMs_ = now;
    }
}

size_t AppManager::appCount() const
{
    return appCount_;
}

const char *AppManager::appNameAt(size_t index) const
{
    if (index >= appCount_)
    {
        return nullptr;
    }

    return apps_[index].name;
}

void AppManager::onEvent(Event *event)
{
    if (event != nullptr && event->type == EventType::EVT_SYSTEM && event->arg0 == SYSTEM_EVENT_BACK)
    {
        if (StateMachine::instance().currentState() == GlobalState::APP_RUNNING)
        {
            destroyActiveApp();
            (void)StateMachine::instance().goBack();
        }
    }
}

void AppManager::destroyActiveApp()
{
    if (activeApp_ == nullptr)
    {
        return;
    }

    activeApp_->onDestroy();
    delete activeApp_;
    activeApp_ = nullptr;
}
