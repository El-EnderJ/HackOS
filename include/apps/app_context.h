/**
 * @file app_context.h
 * @brief Simulated sandboxing for HackOS applications.
 *
 * Since the ESP32 lacks an MMU for true process isolation, AppContext
 * provides resource tracking for every app.  All dynamic memory,
 * event subscriptions, and timer handles allocated by an application
 * are registered here.  When the app is closed (or crashes), the
 * AppManager sweeps this context to release every tracked resource,
 * preventing memory leaks.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/event_system.h"

namespace hackos {

/**
 * @brief Tracks all resources owned by a single application instance.
 *
 * Usage:
 * @code
 * AppContext ctx;
 * void *buf = ctx.alloc(256);
 * ctx.subscribeObserver(&myObserver);
 * // … on shutdown …
 * ctx.releaseAll();   // frees buf, unsubscribes myObserver
 * @endcode
 */
class AppContext
{
public:
    AppContext();

    // ── Dynamic memory tracking ──────────────────────────────────────────

    /**
     * @brief Allocate memory and track it in this context.
     * @param size  Number of bytes to allocate.
     * @return Pointer to the allocated block, or nullptr on failure.
     */
    void *alloc(size_t size);

    /**
     * @brief Free a previously allocated block and remove it from tracking.
     * @param ptr  Pointer returned by alloc().
     */
    void free(void *ptr);

    // ── Event observer tracking ──────────────────────────────────────────

    /**
     * @brief Subscribe an observer and track the subscription.
     * @param observer  Observer to subscribe to the global EventSystem.
     * @return true if the subscription and tracking succeeded.
     */
    bool subscribeObserver(IEventObserver *observer);

    /**
     * @brief Unsubscribe an observer and remove it from tracking.
     * @param observer  A previously subscribed observer.
     */
    void unsubscribeObserver(const IEventObserver *observer);

    // ── Timer tracking (software timer IDs) ──────────────────────────────

    /**
     * @brief Register a timer ID for automatic cleanup.
     * @param timerId  Application-defined timer identifier.
     * @return true if registered, false if tracking table is full.
     */
    bool registerTimer(uint32_t timerId);

    /**
     * @brief Remove a timer from tracking.
     * @param timerId  Previously registered timer identifier.
     */
    void unregisterTimer(uint32_t timerId);

    // ── Bulk cleanup ─────────────────────────────────────────────────────

    /**
     * @brief Release ALL tracked resources.
     *
     * Frees every allocation, unsubscribes every observer, and clears
     * every tracked timer.  Called automatically by AppManager when an
     * app is destroyed.
     */
    void releaseAll();

    // ── Diagnostics ──────────────────────────────────────────────────────

    /// @brief Number of tracked allocations still live.
    size_t allocCount() const;

    /// @brief Number of tracked observers still subscribed.
    size_t observerCount() const;

    /// @brief Number of tracked timers still registered.
    size_t timerCount() const;

private:
    static constexpr size_t MAX_ALLOCS    = 16U;
    static constexpr size_t MAX_OBSERVERS = 4U;
    static constexpr size_t MAX_TIMERS    = 4U;

    void *allocs_[MAX_ALLOCS];
    size_t allocCount_;

    IEventObserver *observers_[MAX_OBSERVERS];
    size_t observerCount_;

    uint32_t timers_[MAX_TIMERS];
    size_t timerCount_;
};

} // namespace hackos
