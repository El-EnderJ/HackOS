/**
 * @file view_dispatcher.cpp
 * @brief ViewDispatcher implementation – routes draw/input to the active view.
 */

#include "ui/view_dispatcher.h"

#include "ui/canvas.h"
#include "ui/view.h"

ViewDispatcher::ViewDispatcher()
    : views_{},
      viewCount_(0U),
      currentView_(nullptr)
{
}

bool ViewDispatcher::addView(uint32_t id, View *view)
{
    if (view == nullptr || viewCount_ >= MAX_VIEWS)
    {
        return false;
    }

    views_[viewCount_].id = id;
    views_[viewCount_].view = view;
    ++viewCount_;
    return true;
}

void ViewDispatcher::removeView(uint32_t id)
{
    for (size_t i = 0U; i < viewCount_; ++i)
    {
        if (views_[i].id == id)
        {
            if (currentView_ == views_[i].view)
            {
                currentView_ = nullptr;
            }

            // Shift remaining entries.
            for (size_t j = i; j + 1U < viewCount_; ++j)
            {
                views_[j] = views_[j + 1U];
            }
            --viewCount_;
            return;
        }
    }
}

void ViewDispatcher::switchToView(uint32_t id)
{
    for (size_t i = 0U; i < viewCount_; ++i)
    {
        if (views_[i].id == id)
        {
            currentView_ = views_[i].view;
            return;
        }
    }
}

void ViewDispatcher::draw(Canvas *canvas)
{
    if (currentView_ != nullptr && canvas != nullptr)
    {
        currentView_->draw(canvas);
    }
}

bool ViewDispatcher::sendInput(InputEvent *event)
{
    if (currentView_ != nullptr && event != nullptr)
    {
        return currentView_->input(event);
    }
    return false;
}

View *ViewDispatcher::currentView() const
{
    return currentView_;
}
