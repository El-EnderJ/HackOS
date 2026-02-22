#pragma once

#include "core/event.h"

class AppBase
{
public:
    virtual ~AppBase() = default;

    virtual void onSetup() = 0;
    virtual void onLoop() = 0;
    virtual void onDraw() = 0;
    virtual void onEvent(Event *event) = 0;
    virtual void onDestroy() = 0;
};
