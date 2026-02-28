#pragma once

#include <cstdint>

enum class EventType : uint8_t
{
    EVT_INPUT,
    EVT_SYSTEM,
    EVT_WIFI_SCAN_DONE,
    EVT_APP,
    EVT_XP_EARNED,
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

/// arg0 values for EVT_XP_EARNED events (XP amount to award).
enum : int32_t
{
    XP_WIFI_SCAN   = 10,
    XP_NFC_READ    = 15,
    XP_IR_SEND     = 10,
    XP_RF_CAPTURE  = 20,
    XP_BLE_SCAN    = 10,
    XP_SUBGHZ_OP   = 25,
    XP_BADBT_RUN   = 20,
    XP_PORTAL_LOOT = 30,
};
