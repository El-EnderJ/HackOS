/**
 * @file theme_manager.cpp
 * @brief ThemeManager – runtime theme switching for HackOS UI.
 */

#include "ui/theme_manager.h"

namespace hackos {
namespace ui {

// ── Theme definitions ────────────────────────────────────────────────────────

const Theme ThemeManager::themes_[THEME_COUNT] = {
    // HackOS_Dark – default dark-background theme.
    {
        /* name            = */ "HackOS_Dark",
        /* foreground      = */ true,
        /* background      = */ false,
        /* invertStatusBar = */ false,
        /* borderWidth     = */ 1U,
        /* itemPaddingY    = */ 1U,
        /* boldHeaders     = */ false,
    },
    // HackOS_HighContrast – inverted status bar, bolder borders.
    {
        /* name            = */ "HackOS_HighContrast",
        /* foreground      = */ true,
        /* background      = */ false,
        /* invertStatusBar = */ true,
        /* borderWidth     = */ 2U,
        /* itemPaddingY    = */ 2U,
        /* boldHeaders     = */ true,
    },
};

// ── Singleton ────────────────────────────────────────────────────────────────

ThemeManager &ThemeManager::instance()
{
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager()
    : activeId_(ThemeId::DARK)
{
}

void ThemeManager::setTheme(ThemeId id)
{
    if (static_cast<size_t>(id) < THEME_COUNT)
    {
        activeId_ = id;
    }
}

void ThemeManager::cycleTheme()
{
    uint8_t next = (static_cast<uint8_t>(activeId_) + 1U) % static_cast<uint8_t>(THEME_COUNT);
    activeId_ = static_cast<ThemeId>(next);
}

const Theme &ThemeManager::theme() const
{
    return themes_[static_cast<size_t>(activeId_)];
}

ThemeManager::ThemeId ThemeManager::currentThemeId() const
{
    return activeId_;
}

} // namespace ui
} // namespace hackos
