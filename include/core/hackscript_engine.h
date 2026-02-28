/**
 * @file hackscript_engine.h
 * @brief HackScript macro engine – automated attack choreography.
 *
 * Parses and executes `.hs` script files from the SD card.  Each line
 * is a command (e.g. WIFI_EVIL_TWIN "SSID", WAIT 5s, BLE_SPAM_APPLE 30s).
 * The engine runs cooperatively: tick() executes one step per call, so
 * the UI remains responsive during long-running scripts.
 *
 * Supported commands:
 *  - WIFI_EVIL_TWIN "<ssid>"   – launch captive portal with given SSID
 *  - WAIT_FOR_CREDENTIAL       – block until EVT_CREDENTIALS received
 *  - STOP_WIFI                 – tear down AP
 *  - BLE_SPAM_APPLE <dur>      – Apple BLE spam for duration (e.g. 30s)
 *  - NOTIFY "<msg>"            – show a toast notification
 *  - WAIT <dur>                – wait for duration (e.g. 5s, 500ms)
 *  - IR_SEND <protocol> <code> – send IR command
 *  - RF_SEND <freq> <code>     – send RF code
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::core {

class HackScriptEngine
{
public:
    static HackScriptEngine &instance();

    /// @brief Load and parse a .hs script file from the SD card.
    /// @param path VFS path to the script (e.g. "/ext/scripts/attack.hs").
    /// @return true if the file was loaded and parsed successfully.
    bool load(const char *path);

    /// @brief Execute the next step of the script.
    /// @return true while the script is still running; false when finished.
    bool tick();

    /// @brief Stop execution and reset state.
    void stop();

    /// @brief Returns true if a script is currently loaded and running.
    bool isRunning() const;

    /// @brief Current line number being executed (1-based).
    size_t currentLine() const;

    /// @brief Total number of lines in the loaded script.
    size_t totalLines() const;

    /// @brief Get the raw text of the current command.
    const char *currentCommand() const;

private:
    static constexpr size_t MAX_LINES    = 64U;
    static constexpr size_t MAX_LINE_LEN = 128U;

    /// @brief Parse duration string like "30s", "500ms", "5".
    static uint32_t parseDuration(const char *str);

    HackScriptEngine();

    /// @brief Execute a single parsed command line.
    void executeCommand(const char *line);

    char lines_[MAX_LINES][MAX_LINE_LEN];
    size_t lineCount_;
    size_t currentLine_;
    bool running_;
    uint32_t waitUntilMs_;
    bool waitingForCredential_;
};

} // namespace hackos::core
