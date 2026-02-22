#include "hardware/storage.h"

#include <SD.h>
#include <SPI.h>

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
