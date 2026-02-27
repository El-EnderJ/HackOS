/**
 * @file view_dispatcher.h
 * @brief Routes draw and input events to the active view.
 *
 * The ViewDispatcher maintains a small registry of views keyed by
 * numeric ID.  At any time exactly one view is active; it receives
 * all draw() and input() calls forwarded by the Gui render loop.
 */

#pragma once

#include <cstddef>
#include <cstdint>

class Canvas;
class View;
struct InputEvent;

class ViewDispatcher
{
public:
    ViewDispatcher();

    /**
     * @brief Register a view under the given identifier.
     * @param id    Unique view identifier chosen by the application.
     * @param view  Pointer to the View (ownership remains with caller).
     * @return true on success, false if the registry is full.
     */
    bool addView(uint32_t id, View *view);

    /**
     * @brief Remove a previously registered view.
     * @param id  Identifier of the view to remove.
     */
    void removeView(uint32_t id);

    /**
     * @brief Make the given view the active one.
     * @param id  Identifier of a registered view.
     */
    void switchToView(uint32_t id);

    /// @brief Ask the active view to draw onto the canvas.
    void draw(Canvas *canvas);

    /// @brief Forward an input event to the active view.
    bool sendInput(InputEvent *event);

    /// @brief Return the currently active view (may be nullptr).
    View *currentView() const;

private:
    static constexpr size_t MAX_VIEWS = 8U;

    struct ViewEntry
    {
        uint32_t id;
        View *view;
    };

    ViewEntry views_[MAX_VIEWS];
    size_t viewCount_;
    View *currentView_;
};
