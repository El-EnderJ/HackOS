#include "hardware/storage.h"

#include <SD.h>
#include <SPI.h>
#include <cstring>

#include "config.h"

StorageManager &StorageManager::instance()
{
    static StorageManager manager;
    return manager;
}

StorageManager::StorageManager()
    : mounted_(false),
      lastError_("Not initialized")
{
}

bool StorageManager::mount()
{
    if (mounted_)
    {
        return true;
    }

    SPI.begin();
    if (!SD.begin(PIN_SD_CS, SPI))
    {
        lastError_ = "Failed to mount SD";
        mounted_ = false;
        return false;
    }

    mounted_ = true;
    lastError_ = "OK";
    return true;
}

void StorageManager::unmount()
{
    if (!mounted_)
    {
        return;
    }

    SD.end();
    SPI.end();
    mounted_ = false;
    lastError_ = "Unmounted";
}

bool StorageManager::isMounted() const
{
    return mounted_;
}

const char *StorageManager::lastError() const
{
    return lastError_;
}

size_t StorageManager::listDir(const char *path, DirEntry *entries, size_t maxEntries)
{
    if (!mounted_ || path == nullptr || entries == nullptr || maxEntries == 0U)
    {
        return 0U;
    }

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        lastError_ = "Not a directory";
        return 0U;
    }

    size_t count = 0U;
    while (count < maxEntries)
    {
        File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        const char *entryName = entry.name();
        std::strncpy(entries[count].name, entryName, sizeof(entries[count].name) - 1U);
        entries[count].name[sizeof(entries[count].name) - 1U] = '\0';
        entries[count].isDir = entry.isDirectory();
        entries[count].size = entry.isDirectory() ? 0U : static_cast<uint32_t>(entry.size());
        entry.close();
        ++count;
    }
    dir.close();
    return count;
}

bool StorageManager::writeFile(const char *path, const uint8_t *data, size_t len)
{
    if (!mounted_ || path == nullptr || (data == nullptr && len > 0U))
    {
        lastError_ = "writeFile: bad args";
        return false;
    }

    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        lastError_ = "writeFile: open failed";
        return false;
    }

    const size_t written = (len > 0U) ? f.write(data, len) : 0U;
    f.close();

    if (written != len)
    {
        lastError_ = "writeFile: short write";
        return false;
    }
    lastError_ = "OK";
    return true;
}

bool StorageManager::appendChunk(const char *path, const uint8_t *data, size_t len)
{
    if (!mounted_ || path == nullptr || data == nullptr || len == 0U)
    {
        lastError_ = "appendChunk: bad args";
        return false;
    }

    File f = SD.open(path, FILE_APPEND);
    if (!f)
    {
        lastError_ = "appendChunk: open failed";
        return false;
    }

    const size_t written = f.write(data, len);
    f.close();

    if (written != len)
    {
        lastError_ = "appendChunk: short write";
        return false;
    }
    lastError_ = "OK";
    return true;
}

bool StorageManager::readFile(const char *path, uint8_t *buf, size_t maxLen, size_t *bytesRead)
{
    if (!mounted_ || path == nullptr || buf == nullptr || maxLen == 0U)
    {
        lastError_ = "readFile: bad args";
        if (bytesRead != nullptr)
        {
            *bytesRead = 0U;
        }
        return false;
    }

    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        lastError_ = "readFile: open failed";
        if (bytesRead != nullptr)
        {
            *bytesRead = 0U;
        }
        return false;
    }

    const size_t fileSize = f.size();
    const size_t toRead = (fileSize < maxLen) ? fileSize : maxLen;
    const size_t nRead = f.read(buf, toRead);
    f.close();

    if (bytesRead != nullptr)
    {
        *bytesRead = nRead;
    }

    if (nRead != toRead)
    {
        lastError_ = "readFile: short read";
        return false;
    }
    lastError_ = "OK";
    return true;
}
