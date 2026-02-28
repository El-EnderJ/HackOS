/**
 * @file plugin_manager.h
 * @brief Dynamic Plugin Manager – load app definitions from JSON on SD card.
 *
 * Scans `/ext/plugins/` for `.json` files, each describing a plugin app.
 * Plugins are registered dynamically with the AppManager and appear in the
 * launcher alongside built-in apps.
 *
 * JSON plugin format:
 * @code
 * {
 *   "name": "my_plugin",
 *   "label": "My Plugin",
 *   "version": "1.0.0",
 *   "author": "Community",
 *   "description": "Does something cool",
 *   "category": "tools",
 *   "icon": "plug",
 *   "config": {
 *     "pin": 25,
 *     "frequency": 433920000,
 *     "protocol": "OOK"
 *   },
 *   "actions": [
 *     {"type": "gpio_toggle", "pin": 25, "label": "Toggle Pin 25"},
 *     {"type": "freq_set", "value": 433920000, "label": "Set 433 MHz"},
 *     {"type": "pwm_tone", "pin": 27, "freq": 1000, "duration": 500, "label": "Beep"}
 *   ]
 * }
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::core {

/// @brief Maximum number of plugins that can be loaded simultaneously.
static constexpr size_t MAX_PLUGINS = 16U;

/// @brief Maximum number of actions per plugin.
static constexpr size_t MAX_PLUGIN_ACTIONS = 8U;

/// @brief Action types supported by the plugin runtime.
enum class PluginActionType : uint8_t
{
    NONE = 0,
    GPIO_TOGGLE,   ///< Toggle a GPIO pin high/low
    GPIO_HIGH,     ///< Set a GPIO pin HIGH
    GPIO_LOW,      ///< Set a GPIO pin LOW
    FREQ_SET,      ///< Set RF frequency (informational)
    PWM_TONE,      ///< Play a PWM tone on a pin
    DELAY_MS,      ///< Delay for N milliseconds
    LOG_MSG,       ///< Print a log message to serial
};

/// @brief A single action that a plugin can perform.
struct PluginAction
{
    PluginActionType type;
    char label[32];
    int32_t pin;
    int32_t value;     ///< Frequency, duration, or generic value
    int32_t duration;  ///< Duration in ms (for PWM_TONE)
};

/// @brief Metadata and runtime state for a loaded plugin.
struct PluginInfo
{
    char name[32];
    char label[48];
    char version[16];
    char author[32];
    char description[96];
    char category[16];
    char filename[64];    ///< JSON filename on SD
    bool enabled;
    bool registered;      ///< Whether registered with AppManager
    PluginAction actions[MAX_PLUGIN_ACTIONS];
    size_t actionCount;
    int32_t configPin;
    int32_t configFrequency;
    char configProtocol[16];
};

// ── PluginManager ────────────────────────────────────────────────────────────

class PluginManager
{
public:
    static PluginManager &instance();

    /**
     * @brief Scan /ext/plugins/ and load all valid JSON plugin definitions.
     * @return Number of plugins loaded.
     */
    size_t scanAndLoad();

    /**
     * @brief Register all enabled plugins with AppManager.
     * @return Number of plugins successfully registered.
     */
    size_t registerAll();

    /**
     * @brief Reload plugins from disk (scan + register, without duplicating).
     * @return Number of new plugins loaded.
     */
    size_t reload();

    /// @brief Number of loaded plugins.
    size_t pluginCount() const;

    /// @brief Access plugin info by index.
    const PluginInfo *pluginAt(size_t index) const;

    /// @brief Find a plugin by name.
    const PluginInfo *findPlugin(const char *name) const;

    /// @brief Enable/disable a plugin.
    bool setEnabled(const char *name, bool enabled);

    /// @brief Delete a plugin's JSON file from SD.
    bool deletePlugin(const char *name);

    /// @brief Execute a specific action of a plugin.
    bool executeAction(const char *pluginName, size_t actionIndex);

private:
    PluginManager();

    /// @brief Parse a single JSON plugin file.
    bool parsePluginFile(const char *path, PluginInfo &info);

    /// @brief Simple JSON string value extractor.
    static bool jsonGetString(const char *json, const char *key, char *out, size_t outSize);

    /// @brief Simple JSON integer value extractor.
    static bool jsonGetInt(const char *json, const char *key, int32_t &out);

    /// @brief Parse the actions array from JSON.
    size_t parseActions(const char *json, PluginAction *actions, size_t maxActions);

    /// @brief Parse a single action type string.
    static PluginActionType parseActionType(const char *typeStr);

    PluginInfo plugins_[MAX_PLUGINS];
    size_t pluginCount_;
};

} // namespace hackos::core
