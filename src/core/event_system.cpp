#include "core/event_system.h"

EventSystem &EventSystem::instance()
{
    static EventSystem system;
    return system;
}

EventSystem::EventSystem()
    : eventQueue_(nullptr),
      observers_{nullptr},
      observerCount_(0U)
{
}

bool EventSystem::init(size_t queueLength)
{
    if (eventQueue_ != nullptr)
    {
        return true;
    }

    eventQueue_ = xQueueCreate(static_cast<UBaseType_t>(queueLength), sizeof(Event));
    return eventQueue_ != nullptr;
}

bool EventSystem::postEvent(const Event &event)
{
    if (eventQueue_ == nullptr)
    {
        return false;
    }

    return xQueueSend(eventQueue_, &event, 0) == pdTRUE;
}

void EventSystem::dispatchPendingEvents()
{
    if (eventQueue_ == nullptr)
    {
        return;
    }

    Event event{};
    while (xQueueReceive(eventQueue_, &event, 0) == pdTRUE)
    {
        for (size_t i = 0; i < observerCount_; ++i)
        {
            if (observers_[i] != nullptr)
            {
                observers_[i]->onEvent(&event);
            }
        }
    }
}

bool EventSystem::subscribe(IEventObserver *observer)
{
    if (observer == nullptr || observerCount_ >= MAX_OBSERVERS)
    {
        return false;
    }

    for (size_t i = 0; i < observerCount_; ++i)
    {
        if (observers_[i] == observer)
        {
            return true;
        }
    }

    observers_[observerCount_] = observer;
    ++observerCount_;
    return true;
}

void EventSystem::unsubscribe(const IEventObserver *observer)
{
    for (size_t i = 0; i < observerCount_; ++i)
    {
        if (observers_[i] == observer)
        {
            for (size_t j = i; j + 1U < observerCount_; ++j)
            {
                observers_[j] = observers_[j + 1U];
            }
            observers_[observerCount_ - 1U] = nullptr;
            --observerCount_;
            return;
        }
    }
}
