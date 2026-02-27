/**
 * @file view.h
 * @brief View base class for the HackOS UI framework.
 *
 * Inspired by the Flipper Zero view model: every screen element
 * is a View that receives draw and input callbacks.
 */

#pragma once

#include <cstdint>

class Canvas;

/// @brief Input event delivered to views by the ViewDispatcher.
struct InputEvent
{
    enum class Type : uint8_t
    {
        UP,
        DOWN,
        LEFT,
        RIGHT,
        CENTER,
        BACK,
    };

    Type type;
};

/**
 * @brief Abstract base for all renderable views.
 *
 * Subclasses override draw() to paint onto the Canvas and input()
 * to handle joystick events.
 */
class View
{
public:
    virtual ~View() = default;

    /**
     * @brief Render the view onto the given canvas.
     * @param canvas  Pointer to the frame-buffer canvas.
     */
    virtual void draw(Canvas *canvas) = 0;

    /**
     * @brief Handle an input event.
     * @param event  Pointer to the input event.
     * @return true if the event was consumed.
     */
    virtual bool input(InputEvent *event) = 0;
};
