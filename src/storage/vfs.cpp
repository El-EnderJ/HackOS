#include "storage/vfs.h"

#include <SD.h>
#include <LittleFS.h>
#include <cstring>
#include <esp_log.h>

#include "hardware/storage.h"

static constexpr const char *TAG_VFS = "VFS";

namespace hackos::storage {

// ── Singleton ────────────────────────────────────────────────────────────────

VirtualFS &VirtualFS::instance()
{
    static VirtualFS vfs;
    return vfs;
}

VirtualFS::VirtualFS()
    : sdMounted_(false),
      flashMounted_(false),
      lastError_("Not initialized")
{
}

// ── Initialisation ───────────────────────────────────────────────────────────

bool VirtualFS::init()
{
    // SD: reuse the mount that StorageManager already performed.
    sdMounted_ = StorageManager::instance().isMounted();
    if (sdMounted_)
    {
        ESP_LOGI(TAG_VFS, "SD card available via StorageManager");
    }
    else
    {
        ESP_LOGW(TAG_VFS, "SD card not mounted – /ext paths unavailable");
    }

    // LittleFS: mount the internal flash partition.
    if (!LittleFS.begin(/* formatOnFail = */ true))
    {
        ESP_LOGE(TAG_VFS, "LittleFS mount failed");
        flashMounted_ = false;
    }
    else
    {
        flashMounted_ = true;
        ESP_LOGI(TAG_VFS, "LittleFS mounted – /int paths available");
    }

    lastError_ = (sdMounted_ || flashMounted_) ? "OK" : "No storage available";
    return sdMounted_ || flashMounted_;
}

// ── Path helpers ─────────────────────────────────────────────────────────────

StorageType VirtualFS::resolveStorage(const char *path)
{
    if (path == nullptr)
    {
        return StorageType::UNKNOWN;
    }
    if (std::strncmp(path, "/ext", 4U) == 0 &&
        (path[4] == '/' || path[4] == '\0'))
    {
        return StorageType::SD_CARD;
    }
    if (std::strncmp(path, "/int", 4U) == 0 &&
        (path[4] == '/' || path[4] == '\0'))
    {
        return StorageType::FLASH;
    }
    return StorageType::UNKNOWN;
}

const char *VirtualFS::stripPrefix(const char *path)
{
    if (path == nullptr)
    {
        return "/";
    }
    // Skip the 4-character prefix ("/ext" or "/int").
    if ((std::strncmp(path, "/ext", 4U) == 0 || std::strncmp(path, "/int", 4U) == 0) &&
        (path[4] == '/' || path[4] == '\0'))
    {
        const char *rest = path + 4;
        return (*rest != '\0') ? rest : "/";
    }
    return path;
}

fs::FS *VirtualFS::getFS(StorageType type)
{
    switch (type)
    {
    case StorageType::SD_CARD:
        return sdMounted_ ? &SD : nullptr;
    case StorageType::FLASH:
        return flashMounted_ ? &LittleFS : nullptr;
    default:
        return nullptr;
    }
}

// ── File operations ──────────────────────────────────────────────────────────

fs::File VirtualFS::open(const char *path, const char *mode)
{
    const StorageType st = resolveStorage(path);
    fs::FS *fs = getFS(st);
    if (fs == nullptr)
    {
        lastError_ = "open: storage unavailable or bad path";
        return fs::File();
    }
    const char *localPath = stripPrefix(path);
    fs::File f = fs->open(localPath, mode);
    if (!f)
    {
        lastError_ = "open: file not found";
    }
    else
    {
        lastError_ = "OK";
    }
    return f;
}

bool VirtualFS::exists(const char *path)
{
    const StorageType st = resolveStorage(path);
    fs::FS *fs = getFS(st);
    if (fs == nullptr)
    {
        lastError_ = "exists: storage unavailable or bad path";
        return false;
    }
    return fs->exists(stripPrefix(path));
}

bool VirtualFS::mkdir(const char *path)
{
    const StorageType st = resolveStorage(path);
    fs::FS *fs = getFS(st);
    if (fs == nullptr)
    {
        lastError_ = "mkdir: storage unavailable or bad path";
        return false;
    }
    const char *localPath = stripPrefix(path);
    if (fs->exists(localPath))
    {
        lastError_ = "OK";
        return true;
    }
    if (!fs->mkdir(localPath))
    {
        lastError_ = "mkdir: failed";
        return false;
    }
    lastError_ = "OK";
    return true;
}

bool VirtualFS::remove(const char *path)
{
    const StorageType st = resolveStorage(path);
    fs::FS *fs = getFS(st);
    if (fs == nullptr)
    {
        lastError_ = "remove: storage unavailable or bad path";
        return false;
    }
    if (!fs->remove(stripPrefix(path)))
    {
        lastError_ = "remove: failed";
        return false;
    }
    lastError_ = "OK";
    return true;
}

// ── Directory listing ────────────────────────────────────────────────────────

size_t VirtualFS::listDir(const char *path, DirEntry *entries, size_t maxEntries)
{
    if (entries == nullptr || maxEntries == 0U)
    {
        return 0U;
    }

    const StorageType st = resolveStorage(path);
    fs::FS *fs = getFS(st);
    if (fs == nullptr)
    {
        lastError_ = "listDir: storage unavailable or bad path";
        return 0U;
    }

    const char *localPath = stripPrefix(path);
    fs::File dir = fs->open(localPath);
    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }
        lastError_ = "listDir: not a directory";
        return 0U;
    }

    size_t count = 0U;
    while (count < maxEntries)
    {
        fs::File entry = dir.openNextFile();
        if (!entry)
        {
            break;
        }
        const char *entryName = entry.name();
        std::strncpy(entries[count].name, entryName,
                      sizeof(entries[count].name) - 1U);
        entries[count].name[sizeof(entries[count].name) - 1U] = '\0';
        entries[count].isDir = entry.isDirectory();
        entries[count].size = entry.isDirectory()
                                  ? 0U
                                  : static_cast<uint32_t>(entry.size());
        entry.close();
        ++count;
    }
    dir.close();
    lastError_ = "OK";
    return count;
}

// ── Status ───────────────────────────────────────────────────────────────────

bool VirtualFS::sdMounted() const { return sdMounted_; }
bool VirtualFS::flashMounted() const { return flashMounted_; }
const char *VirtualFS::lastError() const { return lastError_; }

} // namespace hackos::storage
