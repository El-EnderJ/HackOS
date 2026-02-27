#include "storage/json_parser.h"

#include <cstdlib>
#include <cstring>
#include <esp_log.h>

#include "storage/vfs.h"

static constexpr const char *TAG_JSON = "JsonConfig";

namespace hackos::storage {

// ── Construction ─────────────────────────────────────────────────────────────

JsonConfig::JsonConfig()
    : count_(0U),
      lastError_("Not loaded")
{
    clear();
}

void JsonConfig::clear()
{
    for (size_t i = 0U; i < MAX_KEYS; ++i)
    {
        entries_[i].key[0] = '\0';
        entries_[i].value[0] = '\0';
    }
    count_ = 0U;
}

// ── Loading ──────────────────────────────────────────────────────────────────

bool JsonConfig::loadFromFile(const char *vfsPath)
{
    if (vfsPath == nullptr)
    {
        lastError_ = "loadFromFile: null path";
        return false;
    }

    fs::File f = VirtualFS::instance().open(vfsPath, "r");
    if (!f)
    {
        lastError_ = "loadFromFile: cannot open file";
        return false;
    }

    const size_t fileSize = f.size();
    if (fileSize == 0U || fileSize >= MAX_JSON_SIZE)
    {
        f.close();
        lastError_ = "loadFromFile: file empty or too large";
        return false;
    }

    char buf[MAX_JSON_SIZE];
    const size_t bytesRead = f.read(reinterpret_cast<uint8_t *>(buf), fileSize);
    f.close();

    if (bytesRead != fileSize)
    {
        lastError_ = "loadFromFile: short read";
        return false;
    }
    buf[bytesRead] = '\0';

    return loadFromString(buf);
}

bool JsonConfig::loadFromString(const char *json)
{
    if (json == nullptr)
    {
        lastError_ = "loadFromString: null input";
        return false;
    }
    clear();
    return parse(json);
}

// ── Typed accessors ──────────────────────────────────────────────────────────

const char *JsonConfig::getString(const char *key, const char *defaultVal) const
{
    const Entry *e = findEntry(key);
    return (e != nullptr) ? e->value : defaultVal;
}

int32_t JsonConfig::getInt(const char *key, int32_t defaultVal) const
{
    const Entry *e = findEntry(key);
    if (e == nullptr)
    {
        return defaultVal;
    }
    char *end = nullptr;
    const long val = std::strtol(e->value, &end, 10);
    return (end != e->value) ? static_cast<int32_t>(val) : defaultVal;
}

bool JsonConfig::getBool(const char *key, bool defaultVal) const
{
    const Entry *e = findEntry(key);
    if (e == nullptr)
    {
        return defaultVal;
    }
    if (std::strcmp(e->value, "true") == 0 || std::strcmp(e->value, "1") == 0)
    {
        return true;
    }
    if (std::strcmp(e->value, "false") == 0 || std::strcmp(e->value, "0") == 0)
    {
        return false;
    }
    return defaultVal;
}

bool JsonConfig::hasKey(const char *key) const
{
    return findEntry(key) != nullptr;
}

size_t JsonConfig::keyCount() const { return count_; }

const char *JsonConfig::lastError() const { return lastError_; }

// ── Entry lookup ─────────────────────────────────────────────────────────────

const JsonConfig::Entry *JsonConfig::findEntry(const char *key) const
{
    if (key == nullptr)
    {
        return nullptr;
    }
    for (size_t i = 0U; i < count_; ++i)
    {
        if (std::strcmp(entries_[i].key, key) == 0)
        {
            return &entries_[i];
        }
    }
    return nullptr;
}

// ── Parser ───────────────────────────────────────────────────────────────────

const char *JsonConfig::skipWhitespace(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    {
        ++p;
    }
    return p;
}

const char *JsonConfig::parseString(const char *p, char *out, size_t maxLen)
{
    if (*p != '"')
    {
        return nullptr;
    }
    ++p; // skip opening quote

    size_t i = 0U;
    while (*p != '\0' && *p != '"')
    {
        if (*p == '\\' && *(p + 1) != '\0')
        {
            ++p; // skip backslash, take next char literally
        }
        if (i < maxLen - 1U)
        {
            out[i++] = *p;
        }
        ++p;
    }
    out[i] = '\0';

    if (*p != '"')
    {
        return nullptr; // unterminated string
    }
    return p + 1; // skip closing quote
}

const char *JsonConfig::parseValue(const char *p, char *out, size_t maxLen)
{
    if (*p == '"')
    {
        return parseString(p, out, maxLen);
    }

    // Non-string value: number, boolean, or null.
    size_t i = 0U;
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r')
    {
        if (i < maxLen - 1U)
        {
            out[i++] = *p;
        }
        ++p;
    }
    out[i] = '\0';
    return p;
}

bool JsonConfig::parse(const char *json)
{
    const char *p = skipWhitespace(json);

    if (*p != '{')
    {
        lastError_ = "parse: expected '{'";
        return false;
    }
    ++p;

    while (true)
    {
        p = skipWhitespace(p);
        if (*p == '}')
        {
            lastError_ = "OK";
            return true;
        }

        if (count_ >= MAX_KEYS)
        {
            lastError_ = "parse: too many keys";
            return false;
        }

        // Parse key.
        p = parseString(p, entries_[count_].key, KEY_LEN);
        if (p == nullptr)
        {
            lastError_ = "parse: bad key string";
            return false;
        }

        // Expect ':'.
        p = skipWhitespace(p);
        if (*p != ':')
        {
            lastError_ = "parse: expected ':'";
            return false;
        }
        ++p;

        // Parse value.
        p = skipWhitespace(p);
        p = parseValue(p, entries_[count_].value, VAL_LEN);
        if (p == nullptr)
        {
            lastError_ = "parse: bad value";
            return false;
        }

        ++count_;

        // Expect ',' or '}'.
        p = skipWhitespace(p);
        if (*p == ',')
        {
            ++p;
        }
        else if (*p != '}')
        {
            lastError_ = "parse: expected ',' or '}'";
            return false;
        }
    }
}

} // namespace hackos::storage
