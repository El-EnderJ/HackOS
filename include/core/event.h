#pragma once

#include <cstdint>

enum class EventType : uint8_t
{
    EVT_INPUT,
    EVT_SYSTEM,
    EVT_WIFI_SCAN_DONE,
    EVT_APP,
};

struct Event
{
    EventType type;
    int32_t arg0;
    int32_t arg1;
    void *data;
};

enum : int32_t
{
    SYSTEM_EVENT_BACK = 1,
};

/// arg0 value for EVT_APP events requesting an app launch.
/// arg1 carries the app index registered with AppManager.
enum : int32_t
{
    APP_EVENT_LAUNCH = 1,
};
