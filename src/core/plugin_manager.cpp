/**
 * @file plugin_manager.cpp
 * @brief Dynamic Plugin Manager implementation.
 *
 * Scans `/ext/plugins/` for JSON plugin definitions, parses them, and
 * registers them as dynamic apps with the AppManager.
 */

#include "core/plugin_manager.h"

#include <cstring>
#include <cstdlib>
#include <new>

#include <esp_log.h>
#include <Arduino.h>

#include "apps/app_base.h"
#include "core/app_manager.h"
#include "core/event.h"
#include "core/event_system.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "storage/vfs.h"

static constexpr const char *TAG_PM = "PluginMgr";

namespace hackos::core {

// ═══════════════════════════════════════════════════════════════════════════════
// PluginApp – Generic app created from a plugin definition
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

/// @brief The plugin info pointer for each dynamic app slot.
/// We use a static array to map factory functions to plugin data.
static const PluginInfo *g_pluginSlots[MAX_PLUGINS] = {};

/// @brief Generic plugin app that renders actions on the OLED.
class PluginApp final : public AppBase, public IEventObserver
{
public:
    explicit PluginApp(const PluginInfo *info)
        : info_(info), menuSel_(0U), resultMsg_{}, showResult_(false)
    {
    }

    void onSetup() override
    {
        (void)EventSystem::instance().subscribe(this);
    }

    void onLoop() override {}

    void onDraw() override
    {
        auto &disp = DisplayManager::instance();
        disp.clear();

        // Title
        disp.drawText(0, 0, info_->label);
        disp.drawLine(0, 10, 127, 10);

        if (info_->actionCount == 0U)
        {
            disp.drawText(0, 16, "No actions defined");
            disp.drawText(0, 28, "[BACK] to exit");
        }
        else
        {
            // Draw action menu
            const size_t maxVisible = 4U;
            size_t firstVisible = 0U;
            if (menuSel_ >= maxVisible)
            {
                firstVisible = menuSel_ - maxVisible + 1U;
            }

            for (size_t i = 0U; i < maxVisible && (firstVisible + i) < info_->actionCount; ++i)
            {
                const size_t idx = firstVisible + i;
                const int y = 14 + static_cast<int>(i) * 10;
                if (idx == menuSel_)
                {
                    disp.fillRect(0, y - 1, 128, 9);
                    disp.drawText(2, y, info_->actions[idx].label, 1U, 0U);
                }
                else
                {
                    disp.drawText(2, y, info_->actions[idx].label);
                }
            }
        }

        // Show result message
        if (showResult_)
        {
            disp.drawText(0, 56, resultMsg_);
        }

        disp.present();
    }

    void onEvent(Event *event) override
    {
        if (event == nullptr || event->type != EventType::EVT_INPUT)
        {
            return;
        }

        const auto input = static_cast<InputManager::InputEvent>(event->arg0);

        if (input == InputManager::InputEvent::UP && menuSel_ > 0U)
        {
            --menuSel_;
            showResult_ = false;
        }
        else if (input == InputManager::InputEvent::DOWN &&
                 menuSel_ + 1U < info_->actionCount)
        {
            ++menuSel_;
            showResult_ = false;
        }
        else if (input == InputManager::InputEvent::BUTTON_PRESS)
        {
            executeCurrentAction();
        }
    }

    void onDestroy() override
    {
        EventSystem::instance().unsubscribe(this);
    }

private:
    void executeCurrentAction()
    {
        if (menuSel_ >= info_->actionCount)
        {
            return;
        }

        const PluginAction &act = info_->actions[menuSel_];
        showResult_ = true;

        switch (act.type)
        {
        case PluginActionType::GPIO_TOGGLE:
        {
            pinMode(act.pin, OUTPUT);
            int cur = digitalRead(act.pin);
            digitalWrite(act.pin, cur == HIGH ? LOW : HIGH);
            snprintf(resultMsg_, sizeof(resultMsg_), "Pin %ld toggled", static_cast<long>(act.pin));
            break;
        }
        case PluginActionType::GPIO_HIGH:
            pinMode(act.pin, OUTPUT);
            digitalWrite(act.pin, HIGH);
            snprintf(resultMsg_, sizeof(resultMsg_), "Pin %ld HIGH", static_cast<long>(act.pin));
            break;
        case PluginActionType::GPIO_LOW:
            pinMode(act.pin, OUTPUT);
            digitalWrite(act.pin, LOW);
            snprintf(resultMsg_, sizeof(resultMsg_), "Pin %ld LOW", static_cast<long>(act.pin));
            break;
        case PluginActionType::PWM_TONE:
            ledcSetup(1, act.value, 8);
            ledcAttachPin(act.pin, 1);
            ledcWriteTone(1, act.value);
            if (act.duration > 0)
            {
                delay(act.duration);
                ledcWriteTone(1, 0);
            }
            snprintf(resultMsg_, sizeof(resultMsg_), "Tone %ldHz", static_cast<long>(act.value));
            break;
        case PluginActionType::FREQ_SET:
            snprintf(resultMsg_, sizeof(resultMsg_), "Freq: %ld", static_cast<long>(act.value));
            break;
        case PluginActionType::DELAY_MS:
            delay(act.value > 5000 ? 5000 : act.value);
            snprintf(resultMsg_, sizeof(resultMsg_), "Waited %ldms", static_cast<long>(act.value));
            break;
        case PluginActionType::LOG_MSG:
            ESP_LOGI(TAG_PM, "Plugin log: %s", act.label);
            snprintf(resultMsg_, sizeof(resultMsg_), "Logged OK");
            break;
        default:
            snprintf(resultMsg_, sizeof(resultMsg_), "Unknown action");
            break;
        }

        // Award XP
        EventSystem::instance().postEvent(
            {EventType::EVT_XP_EARNED, XP_PLUGIN_LOAD, 0, nullptr});
    }

    const PluginInfo *info_;
    size_t menuSel_;
    char resultMsg_[32];
    bool showResult_;
};

/// @brief Factory template – creates a PluginApp for slot N.
template <size_t N>
AppBase *createPluginApp()
{
    if (g_pluginSlots[N] != nullptr)
    {
        return new (std::nothrow) PluginApp(g_pluginSlots[N]);
    }
    return nullptr;
}

/// @brief Array of factory functions, one per possible plugin slot.
using FactoryFn = AppBase *(*)();
static const FactoryFn g_pluginFactories[MAX_PLUGINS] = {
    createPluginApp<0>,  createPluginApp<1>,  createPluginApp<2>,  createPluginApp<3>,
    createPluginApp<4>,  createPluginApp<5>,  createPluginApp<6>,  createPluginApp<7>,
    createPluginApp<8>,  createPluginApp<9>,  createPluginApp<10>, createPluginApp<11>,
    createPluginApp<12>, createPluginApp<13>, createPluginApp<14>, createPluginApp<15>,
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// PluginManager
// ═══════════════════════════════════════════════════════════════════════════════

PluginManager &PluginManager::instance()
{
    static PluginManager mgr;
    return mgr;
}

PluginManager::PluginManager()
    : plugins_{}, pluginCount_(0U)
{
}

size_t PluginManager::scanAndLoad()
{
    auto &vfs = storage::VirtualFS::instance();

    if (!vfs.sdMounted())
    {
        ESP_LOGW(TAG_PM, "SD not mounted, skipping plugin scan");
        return 0U;
    }

    storage::VirtualFS::DirEntry entries[MAX_PLUGINS];
    size_t count = vfs.listDir("/ext/plugins", entries, MAX_PLUGINS);

    size_t loaded = 0U;
    for (size_t i = 0U; i < count && pluginCount_ < MAX_PLUGINS; ++i)
    {
        // Only process .json files
        if (entries[i].isDir)
        {
            continue;
        }

        const size_t nameLen = strlen(entries[i].name);
        if (nameLen < 6 || strcmp(entries[i].name + nameLen - 5, ".json") != 0)
        {
            continue;
        }

        // Check for duplicates
        bool duplicate = false;
        for (size_t j = 0U; j < pluginCount_; ++j)
        {
            if (strcmp(plugins_[j].filename, entries[i].name) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        char path[128];
        snprintf(path, sizeof(path), "/ext/plugins/%s", entries[i].name);

        PluginInfo info = {};
        if (parsePluginFile(path, info))
        {
            strncpy(info.filename, entries[i].name, sizeof(info.filename) - 1);
            info.enabled = true;
            plugins_[pluginCount_] = info;
            ++pluginCount_;
            ++loaded;
            ESP_LOGI(TAG_PM, "Loaded plugin: %s (%s)", info.label, info.version);
        }
        else
        {
            ESP_LOGW(TAG_PM, "Failed to parse plugin: %s", entries[i].name);
        }
    }

    ESP_LOGI(TAG_PM, "Scan complete: %u plugins loaded", static_cast<unsigned>(loaded));
    return loaded;
}

size_t PluginManager::registerAll()
{
    auto &appMgr = AppManager::instance();
    size_t registered = 0U;

    for (size_t i = 0U; i < pluginCount_; ++i)
    {
        if (!plugins_[i].enabled || plugins_[i].registered)
        {
            continue;
        }

        if (i >= MAX_PLUGINS)
        {
            break;
        }

        g_pluginSlots[i] = &plugins_[i];
        if (appMgr.registerApp(plugins_[i].name, g_pluginFactories[i]))
        {
            plugins_[i].registered = true;
            ++registered;
            ESP_LOGI(TAG_PM, "Registered plugin app: %s", plugins_[i].name);
        }
        else
        {
            ESP_LOGW(TAG_PM, "Failed to register plugin: %s (AppManager full?)", plugins_[i].name);
        }
    }

    return registered;
}

size_t PluginManager::reload()
{
    size_t newPlugins = scanAndLoad();
    if (newPlugins > 0U)
    {
        registerAll();
    }
    return newPlugins;
}

size_t PluginManager::pluginCount() const
{
    return pluginCount_;
}

const PluginInfo *PluginManager::pluginAt(size_t index) const
{
    if (index >= pluginCount_)
    {
        return nullptr;
    }
    return &plugins_[index];
}

const PluginInfo *PluginManager::findPlugin(const char *name) const
{
    if (name == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0U; i < pluginCount_; ++i)
    {
        if (strcmp(plugins_[i].name, name) == 0)
        {
            return &plugins_[i];
        }
    }
    return nullptr;
}

bool PluginManager::setEnabled(const char *name, bool enabled)
{
    if (name == nullptr)
    {
        return false;
    }
    for (size_t i = 0U; i < pluginCount_; ++i)
    {
        if (strcmp(plugins_[i].name, name) == 0)
        {
            plugins_[i].enabled = enabled;
            return true;
        }
    }
    return false;
}

bool PluginManager::deletePlugin(const char *name)
{
    if (name == nullptr)
    {
        return false;
    }

    for (size_t i = 0U; i < pluginCount_; ++i)
    {
        if (strcmp(plugins_[i].name, name) == 0)
        {
            char path[128];
            snprintf(path, sizeof(path), "/ext/plugins/%s", plugins_[i].filename);

            auto &vfs = storage::VirtualFS::instance();
            if (vfs.remove(path))
            {
                // Shift remaining plugins
                for (size_t j = i; j + 1U < pluginCount_; ++j)
                {
                    plugins_[j] = plugins_[j + 1U];
                }
                --pluginCount_;
                ESP_LOGI(TAG_PM, "Deleted plugin: %s", name);
                return true;
            }
            return false;
        }
    }
    return false;
}

bool PluginManager::executeAction(const char *pluginName, size_t actionIndex)
{
    const PluginInfo *info = findPlugin(pluginName);
    if (info == nullptr || actionIndex >= info->actionCount)
    {
        return false;
    }

    // Action execution is handled by the PluginApp when launched
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// JSON Parsing (lightweight, no external dependency)
// ═══════════════════════════════════════════════════════════════════════════════

bool PluginManager::parsePluginFile(const char *path, PluginInfo &info)
{
    auto &vfs = storage::VirtualFS::instance();
    fs::File file = vfs.open(path, "r");
    if (!file)
    {
        return false;
    }

    // Read entire file (max 2KB)
    static constexpr size_t MAX_JSON_SIZE = 2048U;
    char buf[MAX_JSON_SIZE];
    size_t bytesRead = file.read(reinterpret_cast<uint8_t *>(buf),
                                  sizeof(buf) - 1U);
    file.close();

    if (bytesRead == 0U)
    {
        return false;
    }
    buf[bytesRead] = '\0';

    // Extract required fields
    if (!jsonGetString(buf, "name", info.name, sizeof(info.name)))
    {
        return false;
    }

    // Optional fields with defaults
    if (!jsonGetString(buf, "label", info.label, sizeof(info.label)))
    {
        strncpy(info.label, info.name, sizeof(info.label) - 1);
    }
    if (!jsonGetString(buf, "version", info.version, sizeof(info.version)))
    {
        strncpy(info.version, "1.0.0", sizeof(info.version) - 1);
    }
    if (!jsonGetString(buf, "author", info.author, sizeof(info.author)))
    {
        strncpy(info.author, "Unknown", sizeof(info.author) - 1);
    }
    if (!jsonGetString(buf, "description", info.description, sizeof(info.description)))
    {
        strncpy(info.description, "No description", sizeof(info.description) - 1);
    }
    if (!jsonGetString(buf, "category", info.category, sizeof(info.category)))
    {
        strncpy(info.category, "general", sizeof(info.category) - 1);
    }

    // Config block
    jsonGetInt(buf, "\"pin\"", info.configPin);
    jsonGetInt(buf, "\"frequency\"", info.configFrequency);
    jsonGetString(buf, "protocol", info.configProtocol, sizeof(info.configProtocol));

    // Actions
    info.actionCount = parseActions(buf, info.actions, MAX_PLUGIN_ACTIONS);

    return true;
}

bool PluginManager::jsonGetString(const char *json, const char *key,
                                   char *out, size_t outSize)
{
    char searchKey[48];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);

    const char *pos = strstr(json, searchKey);
    if (pos == nullptr)
    {
        return false;
    }

    // Skip past key and colon
    pos += strlen(searchKey);
    while (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')
    {
        ++pos;
    }

    if (*pos != '"')
    {
        return false;
    }
    ++pos;

    // Copy until closing quote
    size_t i = 0U;
    while (*pos != '\0' && *pos != '"' && i < outSize - 1U)
    {
        if (*pos == '\\' && *(pos + 1) != '\0')
        {
            ++pos; // skip escape
        }
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i > 0U;
}

bool PluginManager::jsonGetInt(const char *json, const char *key, int32_t &out)
{
    const char *pos = strstr(json, key);
    if (pos == nullptr)
    {
        return false;
    }

    pos += strlen(key);
    while (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n' || *pos == '\r')
    {
        ++pos;
    }

    char *end = nullptr;
    long val = strtol(pos, &end, 10);
    if (end == pos)
    {
        return false;
    }

    out = static_cast<int32_t>(val);
    return true;
}

size_t PluginManager::parseActions(const char *json, PluginAction *actions,
                                    size_t maxActions)
{
    const char *arrStart = strstr(json, "\"actions\"");
    if (arrStart == nullptr)
    {
        return 0U;
    }

    arrStart = strchr(arrStart, '[');
    if (arrStart == nullptr)
    {
        return 0U;
    }
    ++arrStart;

    size_t count = 0U;
    const char *pos = arrStart;

    while (count < maxActions)
    {
        // Find next object
        const char *objStart = strchr(pos, '{');
        if (objStart == nullptr)
        {
            break;
        }
        const char *objEnd = strchr(objStart, '}');
        if (objEnd == nullptr)
        {
            break;
        }

        // Extract to a temp buffer for parsing
        size_t objLen = static_cast<size_t>(objEnd - objStart + 1);
        if (objLen >= 256U)
        {
            pos = objEnd + 1;
            continue;
        }

        char objBuf[256];
        memcpy(objBuf, objStart, objLen);
        objBuf[objLen] = '\0';

        PluginAction act = {};
        char typeStr[24] = {};
        jsonGetString(objBuf, "type", typeStr, sizeof(typeStr));
        act.type = parseActionType(typeStr);
        jsonGetString(objBuf, "label", act.label, sizeof(act.label));
        jsonGetInt(objBuf, "\"pin\"", act.pin);
        jsonGetInt(objBuf, "\"value\"", act.value);
        jsonGetInt(objBuf, "\"freq\"", act.value); // freq overwrites value
        jsonGetInt(objBuf, "\"duration\"", act.duration);

        if (act.type != PluginActionType::NONE && act.label[0] != '\0')
        {
            actions[count] = act;
            ++count;
        }

        pos = objEnd + 1;
    }

    return count;
}

PluginActionType PluginManager::parseActionType(const char *typeStr)
{
    if (typeStr == nullptr || typeStr[0] == '\0')
    {
        return PluginActionType::NONE;
    }

    if (strcmp(typeStr, "gpio_toggle") == 0) return PluginActionType::GPIO_TOGGLE;
    if (strcmp(typeStr, "gpio_high") == 0)   return PluginActionType::GPIO_HIGH;
    if (strcmp(typeStr, "gpio_low") == 0)    return PluginActionType::GPIO_LOW;
    if (strcmp(typeStr, "freq_set") == 0)    return PluginActionType::FREQ_SET;
    if (strcmp(typeStr, "pwm_tone") == 0)    return PluginActionType::PWM_TONE;
    if (strcmp(typeStr, "delay_ms") == 0)    return PluginActionType::DELAY_MS;
    if (strcmp(typeStr, "log_msg") == 0)     return PluginActionType::LOG_MSG;

    return PluginActionType::NONE;
}

} // namespace hackos::core
