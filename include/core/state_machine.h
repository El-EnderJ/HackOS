#pragma once

#include <cstddef>

enum class GlobalState
{
    BOOT,
    SPLASH,
    LAUNCHER,
    APP_RUNNING,
};

class StateMachine
{
public:
    static StateMachine &instance();

    void init(GlobalState initialState = GlobalState::BOOT);
    bool pushState(GlobalState state);
    bool goBack();
    GlobalState currentState() const;

private:
    static constexpr size_t MAX_STACK_DEPTH = 8U;

    StateMachine();

    GlobalState stack_[MAX_STACK_DEPTH];
    size_t depth_;
};
