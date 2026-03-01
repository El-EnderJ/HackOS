/**
 * @file lua_engine.cpp
 * @brief Lightweight Lua-like scripting engine implementation.
 *
 * Line-by-line interpreter supporting:
 *  - Variable assignment:  x = 42,  name = "hello"
 *  - Function calls:       display.text(10, 20, "hi"), sleep(1000)
 *  - Conditionals:         if x > 0 then ... end
 *  - For loops:            for i = 1, 10 do ... end
 *  - While loops:          while x < 10 do ... end
 *  - Comments:             -- this is a comment
 */

#include "core/lua_engine.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hardware/display.h"
#include "storage/vfs.h"
#include "ui/toast_manager.h"

static constexpr const char *TAG_LUA = "LuaEngine";

namespace hackos::core {

// ── Result string ────────────────────────────────────────────────────────────

const char *luaResultStr(LuaResult r)
{
    switch (r)
    {
    case LuaResult::OK:                 return "OK";
    case LuaResult::ERR_SYNTAX:         return "Syntax error";
    case LuaResult::ERR_RUNTIME:        return "Runtime error";
    case LuaResult::ERR_STACK_OVERFLOW: return "Stack overflow";
    case LuaResult::ERR_FILE_NOT_FOUND: return "File not found";
    case LuaResult::ERR_FILE_TOO_LARGE: return "File too large";
    case LuaResult::ERR_STOPPED:        return "Stopped";
    }
    return "Unknown";
}

// ── Singleton ────────────────────────────────────────────────────────────────

LuaEngine &LuaEngine::instance()
{
    static LuaEngine inst;
    return inst;
}

LuaEngine::LuaEngine()
    : lineCount_(0U),
      loopDepth_(0U),
      running_(false),
      stopRequested_(false),
      errLine_(0U)
{
    std::memset(scriptBuf_, 0, sizeof(scriptBuf_));
    std::memset(lines_, 0, sizeof(lines_));
    std::memset(vars_, 0, sizeof(vars_));
    std::memset(loopStack_, 0, sizeof(loopStack_));
    std::memset(errMsg_, 0, sizeof(errMsg_));
}

// ── File loading ─────────────────────────────────────────────────────────────

LuaResult LuaEngine::loadFile(const char *path)
{
    auto &vfs = hackos::storage::VirtualFS::instance();

    if (!vfs.exists(path))
    {
        std::strncpy(errMsg_, "File not found", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_FILE_NOT_FOUND;
    }

    fs::File f = vfs.open(path, "r");
    if (!f)
    {
        std::strncpy(errMsg_, "Read failed", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_FILE_NOT_FOUND;
    }

    size_t fileSize = f.size();
    if (fileSize >= LUA_MAX_SCRIPT_SIZE - 1U)
    {
        f.close();
        std::strncpy(errMsg_, "Script too large", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_FILE_TOO_LARGE;
    }

    size_t bytesRead = f.readBytes(scriptBuf_, fileSize);
    f.close();

    scriptBuf_[bytesRead] = '\0';
    splitLines();
    ESP_LOGI(TAG_LUA, "Loaded %s (%u lines)", path, static_cast<unsigned>(lineCount_));
    return LuaResult::OK;
}

LuaResult LuaEngine::loadString(const char *source)
{
    if (source == nullptr)
    {
        return LuaResult::ERR_SYNTAX;
    }

    const size_t len = std::strlen(source);
    if (len >= LUA_MAX_SCRIPT_SIZE - 1U)
    {
        return LuaResult::ERR_FILE_TOO_LARGE;
    }

    std::memcpy(scriptBuf_, source, len + 1U);
    splitLines();
    return LuaResult::OK;
}

// ── Line splitting ───────────────────────────────────────────────────────────

void LuaEngine::splitLines()
{
    lineCount_ = 0U;
    char *p = scriptBuf_;

    while (*p != '\0' && lineCount_ < LUA_MAX_LINES)
    {
        // Skip leading whitespace
        while (*p == ' ' || *p == '\t')
        {
            ++p;
        }

        if (*p == '\0')
        {
            break;
        }

        lines_[lineCount_++] = p;

        // Find end of line
        while (*p != '\0' && *p != '\n' && *p != '\r')
        {
            ++p;
        }

        if (*p == '\r')
        {
            *p++ = '\0';
            if (*p == '\n')
            {
                ++p;
            }
        }
        else if (*p == '\n')
        {
            *p++ = '\0';
        }
    }
}

// ── Execution ────────────────────────────────────────────────────────────────

LuaResult LuaEngine::execute()
{
    running_ = true;
    stopRequested_ = false;
    errLine_ = 0U;
    errMsg_[0] = '\0';

    // Clear variables
    std::memset(vars_, 0, sizeof(vars_));
    loopDepth_ = 0U;

    LuaResult res = executeBlock(0U, lineCount_);

    running_ = false;
    return res;
}

LuaResult LuaEngine::executeBlock(size_t startLine, size_t endLine)
{
    size_t i = startLine;

    while (i < endLine)
    {
        if (stopRequested_)
        {
            return LuaResult::ERR_STOPPED;
        }

        // Yield to FreeRTOS every line to prevent WDT
        vTaskDelay(1);

        const char *line = lines_[i];

        // Skip empty lines and comments
        if (line[0] == '\0' || (line[0] == '-' && line[1] == '-'))
        {
            ++i;
            continue;
        }

        errLine_ = i + 1U; // 1-based

        // Check for control structures
        if (std::strncmp(line, "if ", 3) == 0 || std::strncmp(line, "if(", 3) == 0)
        {
            LuaResult res = handleIf(i, endLine);
            if (res != LuaResult::OK)
            {
                return res;
            }
            continue;
        }

        if (std::strncmp(line, "while ", 6) == 0)
        {
            LuaResult res = handleWhile(i, endLine);
            if (res != LuaResult::OK)
            {
                return res;
            }
            continue;
        }

        if (std::strncmp(line, "for ", 4) == 0)
        {
            LuaResult res = handleFor(i, endLine);
            if (res != LuaResult::OK)
            {
                return res;
            }
            continue;
        }

        // Skip 'end' tokens (handled by control structures)
        if (std::strcmp(line, "end") == 0)
        {
            ++i;
            continue;
        }

        // Try assignment or function call
        LuaResult res = executeLine(line);
        if (res != LuaResult::OK)
        {
            return res;
        }

        ++i;
    }

    return LuaResult::OK;
}

LuaResult LuaEngine::executeLine(const char *line)
{
    // Check for assignment (contains '=' but not '==')
    const char *eq = std::strchr(line, '=');
    if (eq != nullptr && eq[1] != '=' && eq != line && eq[-1] != '!' &&
        eq[-1] != '<' && eq[-1] != '>')
    {
        // But not if it's a function call first (e.g., "display.text()")
        const char *paren = std::strchr(line, '(');
        if (paren == nullptr || paren > eq)
        {
            return handleAssignment(line);
        }
    }

    // Function call
    return handleFunctionCall(line);
}

// ── Assignment ───────────────────────────────────────────────────────────────

LuaResult LuaEngine::handleAssignment(const char *line)
{
    // Parse "varname = expr"
    char varName[LUA_VAR_NAME_LEN + 1U] = {0};
    const char *p = line;

    size_t ni = 0U;
    while (*p != '\0' && *p != '=' && *p != ' ' && ni < LUA_VAR_NAME_LEN)
    {
        varName[ni++] = *p++;
    }
    varName[ni] = '\0';

    // Skip to '='
    while (*p != '\0' && *p != '=')
    {
        ++p;
    }
    if (*p == '=')
    {
        ++p;
    }

    // Skip whitespace
    while (*p == ' ' || *p == '\t')
    {
        ++p;
    }

    LuaVar *var = findVar(varName);
    if (var == nullptr)
    {
        var = createVar(varName);
    }
    if (var == nullptr)
    {
        std::strncpy(errMsg_, "Too many variables", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_RUNTIME;
    }

    // String value?
    if (*p == '"')
    {
        var->isString = true;
        extractString(p, var->strVal, LUA_STR_VAL_LEN);
    }
    else
    {
        var->isString = false;
        var->numVal = evaluateExpr(p);
    }

    return LuaResult::OK;
}

// ── Function call dispatch ───────────────────────────────────────────────────

LuaResult LuaEngine::handleFunctionCall(const char *line)
{
    // Parse "func.name(args)" or "func(args)"
    char funcName[48] = {0};
    const char *p = line;

    size_t fi = 0U;
    while (*p != '\0' && *p != '(' && fi < sizeof(funcName) - 1U)
    {
        if (*p != ' ')
        {
            funcName[fi++] = *p;
        }
        ++p;
    }
    funcName[fi] = '\0';

    // Skip '('
    const char *args = "";
    if (*p == '(')
    {
        ++p;
        args = p;
    }

    return callBuiltin(funcName, args);
}

LuaResult LuaEngine::callBuiltin(const char *funcName, const char *args)
{
    // Display functions
    if (std::strcmp(funcName, "display.clear") == 0)   return fnDisplayClear(args);
    if (std::strcmp(funcName, "display.text") == 0)    return fnDisplayText(args);
    if (std::strcmp(funcName, "display.line") == 0)    return fnDisplayLine(args);
    if (std::strcmp(funcName, "display.rect") == 0)    return fnDisplayRect(args);
    if (std::strcmp(funcName, "display.present") == 0) return fnDisplayPresent(args);

    // System functions
    if (std::strcmp(funcName, "sleep") == 0)   return fnSleep(args);
    if (std::strcmp(funcName, "toast") == 0)   return fnToast(args);
    if (std::strcmp(funcName, "print") == 0)   return fnPrint(args);

    // GPIO functions
    if (std::strcmp(funcName, "gpio.mode") == 0)  return fnGpioMode(args);
    if (std::strcmp(funcName, "gpio.write") == 0) return fnGpioWrite(args);
    if (std::strcmp(funcName, "gpio.read") == 0)  return fnGpioRead(args);

    std::snprintf(errMsg_, sizeof(errMsg_), "Unknown func: %.20s", funcName);
    return LuaResult::ERR_SYNTAX;
}

// ── Display bindings ─────────────────────────────────────────────────────────

LuaResult LuaEngine::fnDisplayClear(const char * /*args*/)
{
    DisplayManager::instance().clear();
    return LuaResult::OK;
}

LuaResult LuaEngine::fnDisplayText(const char *args)
{
    // display.text(x, y, "string")
    int32_t x = parseIntArg(args);
    skipComma(args);
    int32_t y = parseIntArg(args);
    skipComma(args);
    char text[LUA_STR_VAL_LEN + 1U] = {0};
    parseStringArg(args, text, LUA_STR_VAL_LEN);

    DisplayManager::instance().drawText(
        static_cast<int16_t>(x), static_cast<int16_t>(y), text);
    return LuaResult::OK;
}

LuaResult LuaEngine::fnDisplayLine(const char *args)
{
    // display.line(x0, y0, x1, y1)
    int32_t x0 = parseIntArg(args);
    skipComma(args);
    int32_t y0 = parseIntArg(args);
    skipComma(args);
    int32_t x1 = parseIntArg(args);
    skipComma(args);
    int32_t y1 = parseIntArg(args);

    DisplayManager::instance().drawLine(
        static_cast<int16_t>(x0), static_cast<int16_t>(y0),
        static_cast<int16_t>(x1), static_cast<int16_t>(y1));
    return LuaResult::OK;
}

LuaResult LuaEngine::fnDisplayRect(const char *args)
{
    // display.rect(x, y, w, h)
    int32_t x = parseIntArg(args);
    skipComma(args);
    int32_t y = parseIntArg(args);
    skipComma(args);
    int32_t w = parseIntArg(args);
    skipComma(args);
    int32_t h = parseIntArg(args);

    DisplayManager::instance().drawRect(
        static_cast<int16_t>(x), static_cast<int16_t>(y),
        static_cast<int16_t>(w), static_cast<int16_t>(h));
    return LuaResult::OK;
}

LuaResult LuaEngine::fnDisplayPresent(const char * /*args*/)
{
    DisplayManager::instance().present();
    return LuaResult::OK;
}

// ── System bindings ──────────────────────────────────────────────────────────

LuaResult LuaEngine::fnSleep(const char *args)
{
    int32_t ms = parseIntArg(args);
    if (ms > 0 && ms <= 30000)
    {
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(ms)));
    }
    return LuaResult::OK;
}

LuaResult LuaEngine::fnToast(const char *args)
{
    char msg[41] = {0};
    parseStringArg(args, msg, 40U);
    ToastManager::instance().show(msg);
    return LuaResult::OK;
}

LuaResult LuaEngine::fnPrint(const char *args)
{
    char msg[64] = {0};
    parseStringArg(args, msg, 63U);
    Serial.println(msg);
    ESP_LOGI(TAG_LUA, "print: %s", msg);
    return LuaResult::OK;
}

// ── GPIO bindings ────────────────────────────────────────────────────────────

LuaResult LuaEngine::fnGpioMode(const char *args)
{
    // gpio.mode(pin, mode)  mode: "in", "out"
    int32_t pin = parseIntArg(args);
    skipComma(args);
    char mode[8] = {0};
    parseStringArg(args, mode, 7U);

    if (std::strcmp(mode, "out") == 0)
    {
        pinMode(static_cast<uint8_t>(pin), OUTPUT);
    }
    else
    {
        pinMode(static_cast<uint8_t>(pin), INPUT);
    }
    return LuaResult::OK;
}

LuaResult LuaEngine::fnGpioWrite(const char *args)
{
    // gpio.write(pin, value)
    int32_t pin = parseIntArg(args);
    skipComma(args);
    int32_t val = parseIntArg(args);
    digitalWrite(static_cast<uint8_t>(pin), val != 0 ? HIGH : LOW);
    return LuaResult::OK;
}

LuaResult LuaEngine::fnGpioRead(const char *args)
{
    // gpio.read(pin) - stores in special var "_result"
    int32_t pin = parseIntArg(args);
    int32_t val = digitalRead(static_cast<uint8_t>(pin));

    LuaVar *var = findVar("_result");
    if (var == nullptr)
    {
        var = createVar("_result");
    }
    if (var != nullptr)
    {
        var->isString = false;
        var->numVal = val;
    }
    return LuaResult::OK;
}

// ── Control structures ───────────────────────────────────────────────────────

LuaResult LuaEngine::handleIf(size_t &lineIdx, size_t endLine)
{
    const char *line = lines_[lineIdx];

    // Parse condition: "if <cond> then"
    const char *p = line + 3; // skip "if "
    skipWhitespace(p);

    // Find "then"
    const char *thenPos = std::strstr(p, "then");
    if (thenPos == nullptr)
    {
        std::strncpy(errMsg_, "Expected 'then'", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_SYNTAX;
    }

    // Extract condition
    char condBuf[64] = {0};
    size_t condLen = static_cast<size_t>(thenPos - p);
    if (condLen >= sizeof(condBuf))
    {
        condLen = sizeof(condBuf) - 1U;
    }
    std::memcpy(condBuf, p, condLen);
    // Trim trailing whitespace
    while (condLen > 0U && (condBuf[condLen - 1U] == ' ' || condBuf[condLen - 1U] == '\t'))
    {
        condBuf[--condLen] = '\0';
    }

    bool cond = evaluateCondition(condBuf);

    // Find matching 'end'
    size_t endIdx = findMatchingEnd(lineIdx + 1U, endLine);
    if (endIdx >= endLine)
    {
        std::strncpy(errMsg_, "Missing 'end'", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_SYNTAX;
    }

    if (cond)
    {
        LuaResult res = executeBlock(lineIdx + 1U, endIdx);
        if (res != LuaResult::OK)
        {
            return res;
        }
    }

    lineIdx = endIdx + 1U;
    return LuaResult::OK;
}

LuaResult LuaEngine::handleWhile(size_t &lineIdx, size_t endLine)
{
    const char *line = lines_[lineIdx];

    // Parse "while <cond> do"
    const char *p = line + 6; // skip "while "
    skipWhitespace(p);

    const char *doPos = std::strstr(p, "do");
    if (doPos == nullptr)
    {
        std::strncpy(errMsg_, "Expected 'do'", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_SYNTAX;
    }

    char condBuf[64] = {0};
    size_t condLen = static_cast<size_t>(doPos - p);
    if (condLen >= sizeof(condBuf))
    {
        condLen = sizeof(condBuf) - 1U;
    }
    std::memcpy(condBuf, p, condLen);
    while (condLen > 0U && (condBuf[condLen - 1U] == ' ' || condBuf[condLen - 1U] == '\t'))
    {
        condBuf[--condLen] = '\0';
    }

    size_t endIdx = findMatchingEnd(lineIdx + 1U, endLine);
    if (endIdx >= endLine)
    {
        std::strncpy(errMsg_, "Missing 'end'", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_SYNTAX;
    }

    // Execute loop body while condition is true (max 1000 iterations for safety)
    for (int iter = 0; iter < 1000 && evaluateCondition(condBuf); ++iter)
    {
        if (stopRequested_)
        {
            return LuaResult::ERR_STOPPED;
        }

        LuaResult res = executeBlock(lineIdx + 1U, endIdx);
        if (res != LuaResult::OK)
        {
            return res;
        }
    }

    lineIdx = endIdx + 1U;
    return LuaResult::OK;
}

LuaResult LuaEngine::handleFor(size_t &lineIdx, size_t endLine)
{
    const char *line = lines_[lineIdx];

    // Parse "for var = start, stop do" or "for var = start, stop, step do"
    const char *p = line + 4; // skip "for "
    skipWhitespace(p);

    char varName[LUA_VAR_NAME_LEN + 1U] = {0};
    size_t vi = 0U;
    while (*p != '\0' && *p != '=' && *p != ' ' && vi < LUA_VAR_NAME_LEN)
    {
        varName[vi++] = *p++;
    }
    varName[vi] = '\0';

    while (*p == ' ' || *p == '=')
    {
        ++p;
    }

    int32_t startVal = evaluateExpr(p);

    // Skip to comma
    while (*p != '\0' && *p != ',')
    {
        ++p;
    }
    if (*p == ',')
    {
        ++p;
    }
    skipWhitespace(p);

    int32_t endVal = evaluateExpr(p);
    int32_t stepVal = 1;

    // Check for step (second comma)
    const char *comma2 = std::strchr(p, ',');
    if (comma2 != nullptr)
    {
        comma2++;
        while (*comma2 == ' ')
        {
            ++comma2;
        }
        stepVal = evaluateExpr(comma2);
    }

    // Find 'do' and 'end'
    size_t endIdx = findMatchingEnd(lineIdx + 1U, endLine);
    if (endIdx >= endLine)
    {
        std::strncpy(errMsg_, "Missing 'end'", sizeof(errMsg_) - 1U);
        return LuaResult::ERR_SYNTAX;
    }

    LuaVar *var = findVar(varName);
    if (var == nullptr)
    {
        var = createVar(varName);
    }
    if (var == nullptr)
    {
        return LuaResult::ERR_RUNTIME;
    }
    var->isString = false;

    if (stepVal == 0)
    {
        stepVal = 1;
    }

    for (int32_t val = startVal;
         (stepVal > 0) ? (val <= endVal) : (val >= endVal);
         val += stepVal)
    {
        if (stopRequested_)
        {
            return LuaResult::ERR_STOPPED;
        }

        var->numVal = val;
        LuaResult res = executeBlock(lineIdx + 1U, endIdx);
        if (res != LuaResult::OK)
        {
            return res;
        }
    }

    lineIdx = endIdx + 1U;
    return LuaResult::OK;
}

size_t LuaEngine::findMatchingEnd(size_t fromLine, size_t endLine)
{
    int depth = 1;
    for (size_t i = fromLine; i < endLine; ++i)
    {
        const char *l = lines_[i];
        if (std::strncmp(l, "if ", 3) == 0 || std::strncmp(l, "while ", 6) == 0 ||
            std::strncmp(l, "for ", 4) == 0)
        {
            ++depth;
        }
        else if (std::strcmp(l, "end") == 0)
        {
            --depth;
            if (depth == 0)
            {
                return i;
            }
        }
    }
    return endLine; // not found
}

// ── Variable management ──────────────────────────────────────────────────────

LuaVar *LuaEngine::findVar(const char *name)
{
    for (size_t i = 0U; i < LUA_MAX_VARS; ++i)
    {
        if (vars_[i].used && std::strcmp(vars_[i].name, name) == 0)
        {
            return &vars_[i];
        }
    }
    return nullptr;
}

LuaVar *LuaEngine::createVar(const char *name)
{
    for (size_t i = 0U; i < LUA_MAX_VARS; ++i)
    {
        if (!vars_[i].used)
        {
            std::strncpy(vars_[i].name, name, LUA_VAR_NAME_LEN);
            vars_[i].name[LUA_VAR_NAME_LEN] = '\0';
            vars_[i].used = true;
            vars_[i].numVal = 0;
            vars_[i].strVal[0] = '\0';
            vars_[i].isString = false;
            return &vars_[i];
        }
    }
    return nullptr;
}

int32_t LuaEngine::evaluateExpr(const char *expr)
{
    skipWhitespace(expr);

    // Check for variable reference
    if (std::isalpha(static_cast<unsigned char>(*expr)) || *expr == '_')
    {
        char varName[LUA_VAR_NAME_LEN + 1U] = {0};
        size_t vi = 0U;
        const char *p = expr;
        while ((*p != '\0') && (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_') &&
               vi < LUA_VAR_NAME_LEN)
        {
            varName[vi++] = *p++;
        }
        varName[vi] = '\0';

        LuaVar *var = findVar(varName);
        if (var != nullptr && !var->isString)
        {
            int32_t val = var->numVal;

            // Check for simple arithmetic: var + N, var - N, var * N
            skipWhitespace(p);
            if (*p == '+' || *p == '-' || *p == '*')
            {
                char op = *p++;
                skipWhitespace(p);
                int32_t rhs = evaluateExpr(p);
                if (op == '+')      return val + rhs;
                else if (op == '-') return val - rhs;
                else if (op == '*') return val * rhs;
            }
            return val;
        }
    }

    // Number literal (including negative)
    if (std::isdigit(static_cast<unsigned char>(*expr)) || *expr == '-')
    {
        int32_t val = static_cast<int32_t>(std::strtol(expr, nullptr, 10));
        // Check for arithmetic after number
        const char *p = expr;
        if (*p == '-')
        {
            ++p;
        }
        while (std::isdigit(static_cast<unsigned char>(*p)))
        {
            ++p;
        }
        skipWhitespace(p);
        if (*p == '+' || *p == '-' || *p == '*')
        {
            char op = *p++;
            skipWhitespace(p);
            int32_t rhs = evaluateExpr(p);
            if (op == '+')      return val + rhs;
            else if (op == '-') return val - rhs;
            else if (op == '*') return val * rhs;
        }
        return val;
    }

    return 0;
}

bool LuaEngine::evaluateCondition(const char *expr)
{
    skipWhitespace(expr);

    // Find comparison operator
    const char *op = nullptr;
    int opLen = 0;

    // Search for operators: ==, ~=, >=, <=, >, <
    for (const char *p = expr; *p != '\0'; ++p)
    {
        if (p[0] == '=' && p[1] == '=') { op = p; opLen = 2; break; }
        if (p[0] == '~' && p[1] == '=') { op = p; opLen = 2; break; }
        if (p[0] == '>' && p[1] == '=') { op = p; opLen = 2; break; }
        if (p[0] == '<' && p[1] == '=') { op = p; opLen = 2; break; }
        if (p[0] == '>' && p[1] != '=') { op = p; opLen = 1; break; }
        if (p[0] == '<' && p[1] != '=') { op = p; opLen = 1; break; }
    }

    if (op == nullptr)
    {
        // Truthy: non-zero expression
        return evaluateExpr(expr) != 0;
    }

    // Extract LHS (trim trailing whitespace)
    char lhsBuf[32] = {0};
    size_t lhsLen = static_cast<size_t>(op - expr);
    if (lhsLen >= sizeof(lhsBuf))
    {
        lhsLen = sizeof(lhsBuf) - 1U;
    }
    std::memcpy(lhsBuf, expr, lhsLen);
    while (lhsLen > 0U && lhsBuf[lhsLen - 1U] == ' ')
    {
        lhsBuf[--lhsLen] = '\0';
    }

    // Extract RHS
    const char *rhs = op + opLen;
    skipWhitespace(rhs);

    int32_t lVal = evaluateExpr(lhsBuf);
    int32_t rVal = evaluateExpr(rhs);

    if (opLen == 2)
    {
        if (op[0] == '=') return lVal == rVal;
        if (op[0] == '~') return lVal != rVal;
        if (op[0] == '>') return lVal >= rVal;
        if (op[0] == '<') return lVal <= rVal;
    }
    else
    {
        if (op[0] == '>') return lVal > rVal;
        if (op[0] == '<') return lVal < rVal;
    }

    return false;
}

void LuaEngine::extractString(const char *input, char *output, size_t maxLen)
{
    if (*input == '"')
    {
        ++input;
    }
    size_t i = 0U;
    while (*input != '\0' && *input != '"' && i < maxLen)
    {
        output[i++] = *input++;
    }
    output[i] = '\0';
}

// ── Argument parsing helpers ─────────────────────────────────────────────────

int32_t LuaEngine::parseIntArg(const char *&args)
{
    skipWhitespace(args);

    // Check if it's a variable name
    if (std::isalpha(static_cast<unsigned char>(*args)) || *args == '_')
    {
        return evaluateExpr(args);
    }

    int32_t val = static_cast<int32_t>(std::strtol(args, nullptr, 10));
    // Advance past number
    if (*args == '-')
    {
        ++args;
    }
    while (std::isdigit(static_cast<unsigned char>(*args)))
    {
        ++args;
    }
    return val;
}

void LuaEngine::parseStringArg(const char *&args, char *out, size_t maxLen)
{
    skipWhitespace(args);

    // Check if it's a variable name (not a string literal)
    if (*args != '"' && (std::isalpha(static_cast<unsigned char>(*args)) || *args == '_'))
    {
        char varName[LUA_VAR_NAME_LEN + 1U] = {0};
        size_t vi = 0U;
        while (*args != '\0' && *args != ')' && *args != ',' && vi < LUA_VAR_NAME_LEN)
        {
            if (*args != ' ')
            {
                varName[vi++] = *args;
            }
            ++args;
        }
        varName[vi] = '\0';

        LuaVar *var = findVar(varName);
        if (var != nullptr && var->isString)
        {
            std::strncpy(out, var->strVal, maxLen);
            out[maxLen] = '\0';
            return;
        }
        else if (var != nullptr)
        {
            std::snprintf(out, maxLen + 1U, "%ld", static_cast<long>(var->numVal));
            return;
        }
        std::strncpy(out, varName, maxLen);
        out[maxLen] = '\0';
        return;
    }

    if (*args == '"')
    {
        ++args;
        size_t i = 0U;
        while (*args != '\0' && *args != '"' && i < maxLen)
        {
            out[i++] = *args++;
        }
        out[i] = '\0';
        if (*args == '"')
        {
            ++args;
        }
    }
}

void LuaEngine::skipComma(const char *&args)
{
    while (*args == ' ' || *args == ',')
    {
        ++args;
    }
}

void LuaEngine::skipWhitespace(const char *&str)
{
    while (*str == ' ' || *str == '\t')
    {
        ++str;
    }
}

void LuaEngine::requestStop()
{
    stopRequested_ = true;
}

bool LuaEngine::isRunning() const
{
    return running_;
}

size_t LuaEngine::errorLine() const
{
    return errLine_;
}

const char *LuaEngine::errorMsg() const
{
    return errMsg_;
}

} // namespace hackos::core
