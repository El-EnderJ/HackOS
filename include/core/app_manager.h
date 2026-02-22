#pragma once

#include <cstddef>

#include "apps/app_base.h"
#include "core/event_system.h"

class AppManager : public IEventObserver
{
public:
    using AppFactory = AppBase *(*)();

    static AppManager &instance();

    bool init();
    bool registerApp(const char *name, AppFactory factory);
    bool launchApp(const char *name);
    void loop();

    void onEvent(Event *event) override;

private:
    struct AppEntry
    {
        const char *name;
        AppFactory factory;
    };

    static constexpr size_t MAX_APPS = 8U;

    AppManager();

    void destroyActiveApp();

    AppEntry apps_[MAX_APPS];
    size_t appCount_;
    AppBase *activeApp_;
};
