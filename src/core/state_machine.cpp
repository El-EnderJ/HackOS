#include "core/state_machine.h"

StateMachine &StateMachine::instance()
{
    static StateMachine machine;
    return machine;
}

StateMachine::StateMachine()
    : stack_{GlobalState::BOOT},
      depth_(0U)
{
}

void StateMachine::init(GlobalState initialState)
{
    depth_ = 0U;
    stack_[depth_] = initialState;
    depth_ = 1U;
}

bool StateMachine::pushState(GlobalState state)
{
    if (depth_ >= MAX_STACK_DEPTH)
    {
        return false;
    }

    stack_[depth_] = state;
    ++depth_;
    return true;
}

bool StateMachine::goBack()
{
    if (depth_ <= 1U)
    {
        return false;
    }

    --depth_;
    return true;
}

GlobalState StateMachine::currentState() const
{
    if (depth_ == 0U)
    {
        return GlobalState::BOOT;
    }

    return stack_[depth_ - 1U];
}
