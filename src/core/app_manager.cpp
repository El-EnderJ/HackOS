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
      activeApp_(nullptr)
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
            return true;
        }
    }

    return false;
}

void AppManager::loop()
{
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
    activeApp_->onDraw();
}

void AppManager::onEvent(Event *event)
{
    if (activeApp_ != nullptr)
    {
        activeApp_->onEvent(event);
    }

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
