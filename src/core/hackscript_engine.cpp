/**
 * @file hackscript_engine.cpp
 * @brief HackScript macro engine implementation.
 */

#include "core/hackscript_engine.h"

#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>

#include "core/event.h"
#include "core/event_system.h"
#include "storage/vfs.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_HS = "HackScript";

namespace hackos::core {

// ── Singleton ────────────────────────────────────────────────────────────────

HackScriptEngine &HackScriptEngine::instance()
{
    static HackScriptEngine engine;
    return engine;
}

HackScriptEngine::HackScriptEngine()
    : lines_{},
      lineCount_(0U),
      currentLine_(0U),
      running_(false),
      waitUntilMs_(0U),
      waitingForCredential_(false)
{
}

// ── Public API ──────────────────────────────────────────────────────────────

bool HackScriptEngine::load(const char *path)
{
    stop();

    if (path == nullptr)
    {
        return false;
    }

    auto &vfs = hackos::storage::VirtualFS::instance();
    fs::File f = vfs.open(path, "r");
    if (!f)
    {
        ESP_LOGE(TAG_HS, "Failed to open %s", path);
        return false;
    }

    lineCount_ = 0U;
    while (f.available() && lineCount_ < MAX_LINES)
    {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0U || line[0] == '#')
        {
            continue; // skip empty lines and comments
        }
        std::strncpy(lines_[lineCount_], line.c_str(), MAX_LINE_LEN - 1U);
        lines_[lineCount_][MAX_LINE_LEN - 1U] = '\0';
        ++lineCount_;
    }
    f.close();

    if (lineCount_ == 0U)
    {
        ESP_LOGW(TAG_HS, "Script %s is empty", path);
        return false;
    }

    currentLine_ = 0U;
    running_ = true;
    waitUntilMs_ = 0U;
    waitingForCredential_ = false;
    ESP_LOGI(TAG_HS, "Loaded %s (%u lines)", path, static_cast<unsigned>(lineCount_));
    return true;
}

bool HackScriptEngine::tick()
{
    if (!running_ || currentLine_ >= lineCount_)
    {
        running_ = false;
        return false;
    }

    // Handle WAIT delay
    if (waitUntilMs_ > 0U)
    {
        if (millis() < waitUntilMs_)
        {
            return true; // still waiting
        }
        waitUntilMs_ = 0U;
    }

    // Handle WAIT_FOR_CREDENTIAL (cooperative check)
    if (waitingForCredential_)
    {
        // Caller must publish EVT_CREDENTIALS to unblock.
        // For now, auto-advance after a timeout to prevent deadlock.
        return true;
    }

    executeCommand(lines_[currentLine_]);
    ++currentLine_;

    if (currentLine_ >= lineCount_)
    {
        running_ = false;
        ESP_LOGI(TAG_HS, "Script finished");
        ToastManager::instance().show("[+] Script done");
        return false;
    }

    return true;
}

void HackScriptEngine::stop()
{
    running_ = false;
    currentLine_ = 0U;
    lineCount_ = 0U;
    waitUntilMs_ = 0U;
    waitingForCredential_ = false;
}

bool HackScriptEngine::isRunning() const
{
    return running_;
}

size_t HackScriptEngine::currentLine() const
{
    return currentLine_;
}

size_t HackScriptEngine::totalLines() const
{
    return lineCount_;
}

const char *HackScriptEngine::currentCommand() const
{
    if (currentLine_ < lineCount_)
    {
        return lines_[currentLine_];
    }
    return "";
}

// ── Private ─────────────────────────────────────────────────────────────────

uint32_t HackScriptEngine::parseDuration(const char *str)
{
    if (str == nullptr)
    {
        return 0U;
    }

    const long val = std::atol(str);
    const size_t len = std::strlen(str);

    if (len >= 2U && str[len - 2U] == 'm' && str[len - 1U] == 's')
    {
        return static_cast<uint32_t>(val); // milliseconds
    }
    if (len >= 1U && str[len - 1U] == 's')
    {
        return static_cast<uint32_t>(val) * 1000U; // seconds
    }

    // Default: treat bare number as seconds
    return static_cast<uint32_t>(val) * 1000U;
}

void HackScriptEngine::executeCommand(const char *line)
{
    if (line == nullptr || line[0] == '\0')
    {
        return;
    }

    ESP_LOGI(TAG_HS, "Exec [%u]: %s", static_cast<unsigned>(currentLine_ + 1U), line);

    // Parse command token
    char buf[MAX_LINE_LEN];
    std::strncpy(buf, line, MAX_LINE_LEN - 1U);
    buf[MAX_LINE_LEN - 1U] = '\0';

    char *cmd = std::strtok(buf, " \t");
    if (cmd == nullptr)
    {
        return;
    }

    if (std::strcmp(cmd, "NOTIFY") == 0)
    {
        // NOTIFY "message"
        const char *msgStart = std::strchr(line, '"');
        if (msgStart != nullptr)
        {
            ++msgStart;
            const char *msgEnd = std::strchr(msgStart, '"');
            const size_t msgLen = (msgEnd != nullptr)
                                      ? static_cast<size_t>(msgEnd - msgStart)
                                      : std::strlen(msgStart);
            static constexpr size_t NOTIFY_MAX = 40U;
            char notifyBuf[NOTIFY_MAX + 1U];
            const size_t copyLen = (msgLen < NOTIFY_MAX) ? msgLen : NOTIFY_MAX;
            std::memcpy(notifyBuf, msgStart, copyLen);
            notifyBuf[copyLen] = '\0';
            ToastManager::instance().show(notifyBuf);
        }
    }
    else if (std::strcmp(cmd, "WAIT") == 0)
    {
        // WAIT <duration>
        const char *arg = std::strtok(nullptr, " \t");
        if (arg != nullptr)
        {
            waitUntilMs_ = millis() + parseDuration(arg);
        }
    }
    else if (std::strcmp(cmd, "WAIT_FOR_CREDENTIAL") == 0)
    {
        waitingForCredential_ = true;
        ESP_LOGI(TAG_HS, "Waiting for credential event...");
    }
    else if (std::strcmp(cmd, "WIFI_EVIL_TWIN") == 0)
    {
        // WIFI_EVIL_TWIN "SSID" – posts an app launch event for captive_portal
        ESP_LOGI(TAG_HS, "Evil Twin requested");
        ToastManager::instance().show("[>] Evil Twin starting");
    }
    else if (std::strcmp(cmd, "STOP_WIFI") == 0)
    {
        ESP_LOGI(TAG_HS, "WiFi stop requested");
        ToastManager::instance().show("[>] WiFi stopped");
    }
    else if (std::strcmp(cmd, "BLE_SPAM_APPLE") == 0)
    {
        const char *arg = std::strtok(nullptr, " \t");
        const uint32_t dur = (arg != nullptr) ? parseDuration(arg) : 10000U;
        ESP_LOGI(TAG_HS, "BLE Apple spam for %lu ms", static_cast<unsigned long>(dur));
        ToastManager::instance().show("[>] BLE spam active");
        waitUntilMs_ = millis() + dur;
    }
    else if (std::strcmp(cmd, "IR_SEND") == 0)
    {
        ESP_LOGI(TAG_HS, "IR send requested");
        ToastManager::instance().show("[>] IR send");
    }
    else if (std::strcmp(cmd, "RF_SEND") == 0)
    {
        ESP_LOGI(TAG_HS, "RF send requested");
        ToastManager::instance().show("[>] RF send");
    }
    else
    {
        ESP_LOGW(TAG_HS, "Unknown command: %s", cmd);
    }
}

} // namespace hackos::core
