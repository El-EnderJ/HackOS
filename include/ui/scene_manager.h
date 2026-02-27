/**
 * @file scene_manager.h
 * @brief Scene management with historical stack for HackOS applications.
 *
 * Applications interact with "scenes" instead of controlling views
 * directly.  Each scene has on_enter / on_event / on_exit handlers.
 * The SceneManager maintains a navigation stack so that pressing
 * "Back" automatically restores the previous scene.
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Callback invoked when a scene becomes active.
using SceneOnEnter = void (*)(void *context);

/// @brief Callback invoked to deliver an application event to the scene.
/// @return true if the event was consumed.
using SceneOnEvent = bool (*)(void *context, uint32_t eventId);

/// @brief Callback invoked when a scene is about to be deactivated.
using SceneOnExit = void (*)(void *context);

/// @brief Describes the three lifecycle hooks for a single scene.
struct SceneHandler
{
    SceneOnEnter onEnter;
    SceneOnEvent onEvent;
    SceneOnExit onExit;
};

/**
 * @brief Manages scene lifecycle and navigation history.
 *
 * Typical usage:
 * @code
 *   static const SceneHandler handlers[] = {
 *       { mainMenuEnter, mainMenuEvent, mainMenuExit },
 *       { scanEnter,     scanEvent,     scanExit     },
 *   };
 *   SceneManager sm(handlers, 2, appPtr);
 *   sm.navigateTo(0);          // enter main menu
 *   sm.navigateTo(1);          // push scanning scene
 *   sm.navigateBack();         // pop back to main menu
 * @endcode
 */
class SceneManager
{
public:
    /**
     * @param handlers     Array of SceneHandler structs (one per scene).
     * @param handlerCount Number of entries in the array.
     * @param context      Opaque pointer passed to every callback.
     */
    SceneManager(const SceneHandler *handlers, size_t handlerCount, void *context);

    /**
     * @brief Push a new scene onto the stack and enter it.
     * @param sceneId  Index into the handlers array.
     */
    void navigateTo(uint32_t sceneId);

    /**
     * @brief Pop the current scene and re-enter the previous one.
     * @return true if a scene was popped, false if the stack was empty.
     */
    bool navigateBack();

    /**
     * @brief Deliver an application event to the current scene.
     * @return true if the event was consumed.
     */
    bool handleEvent(uint32_t eventId);

    /// @brief Return the scene ID currently on top of the stack.
    uint32_t currentScene() const;

    /// @brief Return true if the navigation stack is empty.
    bool empty() const;

private:
    static constexpr size_t MAX_STACK = 8U;

    const SceneHandler *handlers_;
    size_t handlerCount_;
    void *context_;

    uint32_t stack_[MAX_STACK];
    size_t stackDepth_;
};
