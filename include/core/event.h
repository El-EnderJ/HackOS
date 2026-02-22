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
