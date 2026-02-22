#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "core/event.h"

class IEventObserver
{
public:
    virtual ~IEventObserver() = default;
    virtual void onEvent(Event *event) = 0;
};

class EventSystem
{
public:
    static EventSystem &instance();

    bool init(size_t queueLength = 16U);
    bool postEvent(const Event &event);
    void dispatchPendingEvents();

    bool subscribe(IEventObserver *observer);
    void unsubscribe(const IEventObserver *observer);

private:
    static constexpr size_t MAX_OBSERVERS = 8U;

    EventSystem();

    QueueHandle_t eventQueue_;
    IEventObserver *observers_[MAX_OBSERVERS];
    size_t observerCount_;
};
