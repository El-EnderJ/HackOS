/**
 * @file theme_manager.h
 * @brief Theme system for HackOS with Dark and HighContrast modes.
 *
 * On a monochrome SSD1306, "theming" controls the logical foreground/
 * background colours and UI element styling rules (inverted status bar,
 * border thickness, element spacing, etc.).
 *
 * Usage:
 * @code
 *   auto &tm = hackos::ui::ThemeManager::instance();
 *   tm.setTheme(ThemeManager::ThemeId::HIGH_CONTRAST);
 *   bool fg = tm.theme().foreground;  // pixel-on value
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos {
namespace ui {

/// Describes the visual parameters of a theme.
struct Theme
{
    const char *name;         ///< Human-readable name.
    bool foreground;          ///< Pixel value for "on" elements.
    bool background;          ///< Pixel value for "off" areas.
    bool invertStatusBar;     ///< Draw the top bar with inverted colours.
    uint8_t borderWidth;      ///< Border width for cards/panels (px).
    uint8_t itemPaddingY;     ///< Vertical padding inside menu items.
    bool boldHeaders;         ///< Double-strike headers (simulated bold).
};

class ThemeManager
{
public:
    enum class ThemeId : uint8_t
    {
        DARK = 0U,
        HIGH_CONTRAST,
        COUNT_,
    };

    static ThemeManager &instance();

    /// @brief Set the active theme.
    void setTheme(ThemeId id);

    /// @brief Toggle between available themes.
    void cycleTheme();

    /// @brief Access the current theme descriptor.
    const Theme &theme() const;

    /// @brief Current theme identifier.
    ThemeId currentThemeId() const;

private:
    ThemeManager();

    static constexpr size_t THEME_COUNT = static_cast<size_t>(ThemeId::COUNT_);

    static const Theme themes_[THEME_COUNT];
    ThemeId activeId_;
};

} // namespace ui
} // namespace hackos
