#include "apps/app_context.h"

#include <cstdlib>
#include <cstring>

namespace hackos {

AppContext::AppContext()
    : allocs_{},
      allocCount_(0U),
      observers_{},
      observerCount_(0U),
      timers_{},
      timerCount_(0U)
{
}

// ── Dynamic memory ───────────────────────────────────────────────────────────

void *AppContext::alloc(size_t size)
{
    if (size == 0U || allocCount_ >= MAX_ALLOCS)
    {
        return nullptr;
    }

    void *ptr = std::malloc(size);
    if (ptr != nullptr)
    {
        allocs_[allocCount_] = ptr;
        ++allocCount_;
    }
    return ptr;
}

void AppContext::free(void *ptr)
{
    if (ptr == nullptr)
    {
        return;
    }

    for (size_t i = 0U; i < allocCount_; ++i)
    {
        if (allocs_[i] == ptr)
        {
            std::free(ptr);
            // Compact the array
            allocs_[i] = allocs_[allocCount_ - 1U];
            allocs_[allocCount_ - 1U] = nullptr;
            --allocCount_;
            return;
        }
    }
}

// ── Event observer tracking ──────────────────────────────────────────────────

bool AppContext::subscribeObserver(IEventObserver *observer)
{
    if (observer == nullptr || observerCount_ >= MAX_OBSERVERS)
    {
        return false;
    }

    if (!EventSystem::instance().subscribe(observer))
    {
        return false;
    }

    observers_[observerCount_] = observer;
    ++observerCount_;
    return true;
}

void AppContext::unsubscribeObserver(const IEventObserver *observer)
{
    if (observer == nullptr)
    {
        return;
    }

    for (size_t i = 0U; i < observerCount_; ++i)
    {
        if (observers_[i] == observer)
        {
            EventSystem::instance().unsubscribe(observer);
            observers_[i] = observers_[observerCount_ - 1U];
            observers_[observerCount_ - 1U] = nullptr;
            --observerCount_;
            return;
        }
    }
}

// ── Timer tracking ───────────────────────────────────────────────────────────

bool AppContext::registerTimer(uint32_t timerId)
{
    if (timerCount_ >= MAX_TIMERS)
    {
        return false;
    }

    timers_[timerCount_] = timerId;
    ++timerCount_;
    return true;
}

void AppContext::unregisterTimer(uint32_t timerId)
{
    for (size_t i = 0U; i < timerCount_; ++i)
    {
        if (timers_[i] == timerId)
        {
            timers_[i] = timers_[timerCount_ - 1U];
            --timerCount_;
            return;
        }
    }
}

// ── Bulk cleanup ─────────────────────────────────────────────────────────────

void AppContext::releaseAll()
{
    // Free all tracked allocations
    for (size_t i = 0U; i < allocCount_; ++i)
    {
        std::free(allocs_[i]);
        allocs_[i] = nullptr;
    }
    allocCount_ = 0U;

    // Unsubscribe all tracked observers
    for (size_t i = 0U; i < observerCount_; ++i)
    {
        EventSystem::instance().unsubscribe(observers_[i]);
        observers_[i] = nullptr;
    }
    observerCount_ = 0U;

    // Clear tracked timers
    timerCount_ = 0U;
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

size_t AppContext::allocCount() const
{
    return allocCount_;
}

size_t AppContext::observerCount() const
{
    return observerCount_;
}

size_t AppContext::timerCount() const
{
    return timerCount_;
}

} // namespace hackos
