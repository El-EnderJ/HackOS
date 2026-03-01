/**
 * @file lua_engine.h
 * @brief Lightweight Lua-like scripting engine for HackOS.
 *
 * Provides a minimal embedded interpreter for user scripts stored on SD
 * under `/ext/lua/`.  Scripts use a Lua-inspired syntax with bindings for:
 *   - display.*  (clear, text, line, rect, present)
 *   - input.*    (read)
 *   - toast()    (show toast notification)
 *   - sleep()    (delay in milliseconds)
 *   - gpio.*     (read, write, mode)
 *   - print()    (serial output)
 *
 * The engine is intentionally minimal (~4 KB RAM) to fit ESP32 constraints.
 * Scripts are executed line-by-line with basic variable support, conditionals,
 * loops, and function calls.
 *
 * Example script (`/ext/lua/hello.lua`):
 * @code
 *   -- Hello World for HackOS
 *   display.clear()
 *   display.text(10, 20, "Hello HackOS!")
 *   display.present()
 *   sleep(2000)
 *   toast("Script done!")
 * @endcode
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace hackos::core {

/// @brief Maximum script size in bytes.
static constexpr size_t LUA_MAX_SCRIPT_SIZE = 4096U;

/// @brief Maximum number of variables in a script.
static constexpr size_t LUA_MAX_VARS = 32U;

/// @brief Maximum variable name length.
static constexpr size_t LUA_VAR_NAME_LEN = 16U;

/// @brief Maximum string value length.
static constexpr size_t LUA_STR_VAL_LEN = 40U;

/// @brief Maximum number of lines in a script.
static constexpr size_t LUA_MAX_LINES = 128U;

/// @brief Maximum loop nesting depth.
static constexpr size_t LUA_MAX_LOOP_DEPTH = 4U;

/// @brief Variable value (number or string).
struct LuaVar
{
    char name[LUA_VAR_NAME_LEN + 1U];
    int32_t numVal;
    char strVal[LUA_STR_VAL_LEN + 1U];
    bool isString;
    bool used;
};

/// @brief Loop context for while/for tracking.
struct LuaLoop
{
    size_t startLine;    ///< Line index of the loop start
    char varName[LUA_VAR_NAME_LEN + 1U]; ///< Iterator variable (for loops)
    int32_t endVal;      ///< End value (for loops)
    int32_t stepVal;     ///< Step value (for loops)
    bool isWhile;        ///< true = while loop, false = for loop
};

/// @brief Execution result codes.
enum class LuaResult : uint8_t
{
    OK = 0,
    ERR_SYNTAX,
    ERR_RUNTIME,
    ERR_STACK_OVERFLOW,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_TOO_LARGE,
    ERR_STOPPED,
};

/// @brief Returns a human-readable error string.
const char *luaResultStr(LuaResult r);

// ── LuaEngine ────────────────────────────────────────────────────────────────

class LuaEngine
{
public:
    static LuaEngine &instance();

    /**
     * @brief Load a script from the SD card.
     * @param path  Full VFS path (e.g. "/ext/lua/hello.lua").
     * @return LuaResult::OK on success.
     */
    LuaResult loadFile(const char *path);

    /**
     * @brief Load a script from a string buffer.
     * @param source  Null-terminated script source.
     * @return LuaResult::OK on success.
     */
    LuaResult loadString(const char *source);

    /**
     * @brief Execute the loaded script.
     * @return LuaResult::OK on success.
     */
    LuaResult execute();

    /**
     * @brief Request the script to stop (cooperative).
     */
    void requestStop();

    /// @brief Returns true if a script is currently executing.
    bool isRunning() const;

    /// @brief Returns the last error line number (1-based).
    size_t errorLine() const;

    /// @brief Returns the last error message.
    const char *errorMsg() const;

private:
    LuaEngine();

    // ── Parsing ──────────────────────────────────────────────────────────

    void splitLines();
    LuaResult executeLine(const char *line);
    LuaResult executeBlock(size_t startLine, size_t endLine);

    // ── Statement handlers ───────────────────────────────────────────────

    LuaResult handleAssignment(const char *line);
    LuaResult handleFunctionCall(const char *line);
    LuaResult handleIf(size_t &lineIdx, size_t endLine);
    LuaResult handleWhile(size_t &lineIdx, size_t endLine);
    LuaResult handleFor(size_t &lineIdx, size_t endLine);
    size_t findMatchingEnd(size_t fromLine, size_t endLine);

    // ── Built-in function dispatch ───────────────────────────────────────

    LuaResult callBuiltin(const char *funcName, const char *args);

    // Display bindings
    LuaResult fnDisplayClear(const char *args);
    LuaResult fnDisplayText(const char *args);
    LuaResult fnDisplayLine(const char *args);
    LuaResult fnDisplayRect(const char *args);
    LuaResult fnDisplayPresent(const char *args);

    // System bindings
    LuaResult fnSleep(const char *args);
    LuaResult fnToast(const char *args);
    LuaResult fnPrint(const char *args);

    // GPIO bindings
    LuaResult fnGpioMode(const char *args);
    LuaResult fnGpioWrite(const char *args);
    LuaResult fnGpioRead(const char *args);

    // ── Variable management ──────────────────────────────────────────────

    LuaVar *findVar(const char *name);
    LuaVar *createVar(const char *name);
    int32_t evaluateExpr(const char *expr);
    bool evaluateCondition(const char *expr);
    void extractString(const char *input, char *output, size_t maxLen);

    // ── Argument parsing helpers ─────────────────────────────────────────

    int32_t parseIntArg(const char *&args);
    void parseStringArg(const char *&args, char *out, size_t maxLen);
    void skipComma(const char *&args);
    void skipWhitespace(const char *&str);

    // ── State ────────────────────────────────────────────────────────────

    char scriptBuf_[LUA_MAX_SCRIPT_SIZE];
    const char *lines_[LUA_MAX_LINES];
    size_t lineCount_;

    LuaVar vars_[LUA_MAX_VARS];

    LuaLoop loopStack_[LUA_MAX_LOOP_DEPTH];
    size_t loopDepth_;

    bool running_;
    bool stopRequested_;

    size_t errLine_;
    char errMsg_[64];
};

} // namespace hackos::core
