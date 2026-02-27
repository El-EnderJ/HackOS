/**
 * @file scene_manager.cpp
 * @brief SceneManager implementation – lifecycle and navigation stack.
 */

#include "ui/scene_manager.h"

SceneManager::SceneManager(const SceneHandler *handlers, size_t handlerCount, void *context)
    : handlers_(handlers),
      handlerCount_(handlerCount),
      context_(context),
      stack_{},
      stackDepth_(0U)
{
}

void SceneManager::navigateTo(uint32_t sceneId)
{
    if (sceneId >= handlerCount_ || stackDepth_ >= MAX_STACK)
    {
        return;
    }

    // Exit the current scene (if any).
    if (stackDepth_ > 0U)
    {
        uint32_t current = stack_[stackDepth_ - 1U];
        if (handlers_[current].onExit != nullptr)
        {
            handlers_[current].onExit(context_);
        }
    }

    // Push new scene.
    stack_[stackDepth_] = sceneId;
    ++stackDepth_;

    if (handlers_[sceneId].onEnter != nullptr)
    {
        handlers_[sceneId].onEnter(context_);
    }
}

bool SceneManager::navigateBack()
{
    if (stackDepth_ == 0U)
    {
        return false;
    }

    // Exit current scene.
    uint32_t current = stack_[stackDepth_ - 1U];
    if (handlers_[current].onExit != nullptr)
    {
        handlers_[current].onExit(context_);
    }
    --stackDepth_;

    // Re-enter the previous scene.
    if (stackDepth_ > 0U)
    {
        uint32_t prev = stack_[stackDepth_ - 1U];
        if (handlers_[prev].onEnter != nullptr)
        {
            handlers_[prev].onEnter(context_);
        }
    }

    return true;
}

bool SceneManager::handleEvent(uint32_t eventId)
{
    if (stackDepth_ == 0U)
    {
        return false;
    }

    uint32_t current = stack_[stackDepth_ - 1U];
    if (handlers_[current].onEvent != nullptr)
    {
        return handlers_[current].onEvent(context_, eventId);
    }
    return false;
}

uint32_t SceneManager::currentScene() const
{
    if (stackDepth_ == 0U)
    {
        return UINT32_MAX;
    }
    return stack_[stackDepth_ - 1U];
}

bool SceneManager::empty() const
{
    return stackDepth_ == 0U;
}
