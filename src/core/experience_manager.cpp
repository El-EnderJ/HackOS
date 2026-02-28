#include "core/experience_manager.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>

#include "core/event.h"
#include "storage/vfs.h"

static constexpr const char *TAG_XP = "XPManager";

// ── Singleton ────────────────────────────────────────────────────────────────

ExperienceManager &ExperienceManager::instance()
{
    static ExperienceManager mgr;
    return mgr;
}

ExperienceManager::ExperienceManager()
    : level_(1U),
      xp_(0U),
      hackPoints_(0U),
      leveledUp_(false),
      initialized_(false)
{
}

// ── Init / Persistence ──────────────────────────────────────────────────────

bool ExperienceManager::init()
{
    if (initialized_)
    {
        return true;
    }

    load();
    (void)EventSystem::instance().subscribe(this);
    initialized_ = true;
    ESP_LOGI(TAG_XP, "XP system ready – Lv%u  XP=%lu  HP=%lu",
             static_cast<unsigned>(level_),
             static_cast<unsigned long>(xp_),
             static_cast<unsigned long>(hackPoints_));
    return true;
}

void ExperienceManager::load()
{
    auto &vfs = hackos::storage::VirtualFS::instance();

    if (!vfs.flashMounted())
    {
        ESP_LOGW(TAG_XP, "Flash not mounted – starting fresh");
        return;
    }

    fs::File f = vfs.open(STATS_PATH, "r");
    if (!f)
    {
        ESP_LOGI(TAG_XP, "No saved stats – starting fresh");
        return;
    }

    // Simple JSON parse: {"level":N,"xp":N,"hp":N}
    char buf[128];
    const size_t len = f.readBytes(buf, sizeof(buf) - 1U);
    f.close();
    buf[len] = '\0';

    uint32_t lvl = 0U;
    uint32_t xp  = 0U;
    uint32_t hp  = 0U;

    const char *p = std::strstr(buf, "\"level\":");
    if (p != nullptr)
    {
        lvl = static_cast<uint32_t>(std::strtoul(p + 8, nullptr, 10));
    }
    p = std::strstr(buf, "\"xp\":");
    if (p != nullptr)
    {
        xp = static_cast<uint32_t>(std::strtoul(p + 5, nullptr, 10));
    }
    p = std::strstr(buf, "\"hp\":");
    if (p != nullptr)
    {
        hp = static_cast<uint32_t>(std::strtoul(p + 5, nullptr, 10));
    }

    level_      = (lvl >= 1U && lvl <= 999U) ? static_cast<uint16_t>(lvl) : 1U;
    xp_         = xp;
    hackPoints_ = hp;

    ESP_LOGI(TAG_XP, "Loaded stats from flash");
}

void ExperienceManager::save()
{
    auto &vfs = hackos::storage::VirtualFS::instance();

    if (!vfs.flashMounted())
    {
        ESP_LOGW(TAG_XP, "Flash not mounted – cannot save");
        return;
    }

    char buf[128];
    const int n = std::snprintf(buf, sizeof(buf),
                                "{\"level\":%u,\"xp\":%lu,\"hp\":%lu}",
                                static_cast<unsigned>(level_),
                                static_cast<unsigned long>(xp_),
                                static_cast<unsigned long>(hackPoints_));

    fs::File f = vfs.open(STATS_PATH, "w");
    if (!f)
    {
        ESP_LOGE(TAG_XP, "Failed to open stats file for writing");
        return;
    }

    f.write(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n));
    f.close();
    ESP_LOGI(TAG_XP, "Stats saved");
}

// ── XP logic ────────────────────────────────────────────────────────────────

uint32_t ExperienceManager::xpForNextLevel() const
{
    return xpThreshold(level_);
}

void ExperienceManager::addXP(uint32_t amount)
{
    xp_ += amount;
    hackPoints_ += amount / 2U;
    checkLevelUp();
    save();
}

void ExperienceManager::checkLevelUp()
{
    while (xp_ >= xpForNextLevel())
    {
        xp_ -= xpForNextLevel();
        ++level_;
        leveledUp_ = true;
        ESP_LOGI(TAG_XP, "*** LEVEL UP! Now Lv%u ***", static_cast<unsigned>(level_));
    }
}

// ── Event handling ──────────────────────────────────────────────────────────

void ExperienceManager::onEvent(Event *event)
{
    if (event == nullptr || event->type != EventType::EVT_XP_EARNED)
    {
        return;
    }

    const uint32_t amount = (event->arg0 > 0) ? static_cast<uint32_t>(event->arg0) : 0U;
    if (amount == 0U)
    {
        return;
    }

    ESP_LOGI(TAG_XP, "+%lu XP earned", static_cast<unsigned long>(amount));
    addXP(amount);
}
