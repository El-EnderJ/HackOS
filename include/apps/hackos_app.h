/**
 * @file hackos_app.h
 * @brief HackOS App Lifecycle – the strict base class for user-space apps.
 *
 * Every developer-facing application MUST extend HackOSApp and implement
 * the four lifecycle methods:
 *
 *  1. **on_alloc()**  – Request memory and allocate resources through the
 *                       AppContext.  Called once before the UI is created.
 *
 *  2. **on_start()**  – Initialise views, scenes, and subscribe to events.
 *                       The GUI is available at this point.
 *
 *  3. **on_event()**  – Handle input or system messages forwarded by the
 *                       core.
 *
 *  4. **on_free()**   – Clean up everything.  Any resource NOT freed here
 *                       will be swept automatically by AppContext::releaseAll().
 *
 * HackOSApp extends the legacy AppBase so it integrates transparently with
 * the existing AppManager.  The adapter layer maps the old lifecycle
 * (onSetup / onLoop / onDraw / onEvent / onDestroy) to the new four-phase
 * model.
 *
 * @code
 * class MyApp : public hackos::HackOSApp {
 * protected:
 *     void on_alloc() override { buf_ = ctx().alloc(128); }
 *     void on_start() override { // set up views
 *     }
 *     void on_event(Event *e) override { // handle input
 *     }
 *     void on_free()  override { ctx().free(buf_); }
 * private:
 *     void *buf_ = nullptr;
 * };
 * @endcode
 */

#pragma once

#include "apps/app_base.h"
#include "apps/app_context.h"
#include "core/event.h"
#include "core/event_system.h"

namespace hackos {

/**
 * @brief Base class for all user-space HackOS applications.
 *
 * Provides automatic sandboxing via AppContext and adapts to the legacy
 * AppBase interface expected by AppManager.
 */
class HackOSApp : public AppBase, public IEventObserver
{
public:
    ~HackOSApp() override = default;

    // ── New lifecycle API (override these) ────────────────────────────────

    /**
     * @brief Phase 1: Allocate memory and resources via ctx().
     *
     * Called once when the app is first created.  Use ctx().alloc() to
     * request tracked memory.
     */
    virtual void on_alloc() = 0;

    /**
     * @brief Phase 2: Initialise views, scenes, and event subscriptions.
     *
     * Called after on_alloc().  The display and GUI subsystems are ready.
     */
    virtual void on_start() = 0;

    /**
     * @brief Phase 3: Handle an incoming event (input, system, radio, …).
     * @param event  Pointer to the event structure (never nullptr).
     */
    virtual void on_event(Event *event) = 0;

    /**
     * @brief Phase 4: Release all resources.
     *
     * Called before the app is destroyed.  Any tracked resource that the
     * app forgets to free is swept by the AppContext safety net.
     */
    virtual void on_free() = 0;

    // ── Optional hooks ───────────────────────────────────────────────────

    /**
     * @brief Called once per main-loop iteration (for polling, animations).
     *
     * Default implementation does nothing.  Override only if needed.
     */
    virtual void on_update() {}

    /**
     * @brief Called at ~30 FPS to render the UI.
     *
     * Default implementation does nothing.  Override if the app manages
     * its own draw loop (most apps using ViewDispatcher don't need this).
     */
    virtual void on_draw() {}

    // ── AppContext access ────────────────────────────────────────────────

    /// @brief Returns the application's sandboxed resource context.
    AppContext &ctx() { return context_; }

protected:
    HackOSApp() = default;

private:
    AppContext context_;

    // ── Legacy AppBase adapter ───────────────────────────────────────────

    void onSetup() final
    {
        on_alloc();
        context_.subscribeObserver(this);
        on_start();
    }

    void onLoop() final
    {
        on_update();
    }

    void onDraw() final
    {
        on_draw();
    }

    void onEvent(Event *event) final
    {
        if (event != nullptr)
        {
            on_event(event);
        }
    }

    void onDestroy() final
    {
        on_free();
        context_.releaseAll();
    }
};

} // namespace hackos
