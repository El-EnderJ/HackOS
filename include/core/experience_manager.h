/**
 * @file experience_manager.h
 * @brief Gamification layer – tracks XP, level and Hack-Points.
 *
 * The ExperienceManager singleton listens for EVT_XP_EARNED events,
 * updates persistent stats on internal flash (LittleFS via VFS), and
 * exposes the current level / XP for UI components such as the
 * HackBot mascot.
 */

#pragma once

#include <cstdint>

#include "core/event_system.h"

class ExperienceManager : public IEventObserver
{
public:
    static ExperienceManager &instance();

    /// @brief Initialise the manager and load persisted stats from flash.
    bool init();

    /// @brief IEventObserver – processes EVT_XP_EARNED events.
    void onEvent(Event *event) override;

    // ── Accessors ────────────────────────────────────────────────────────

    uint16_t level() const { return level_; }
    uint32_t xp() const { return xp_; }
    uint32_t xpForNextLevel() const;
    uint32_t hackPoints() const { return hackPoints_; }

    /// @brief True if a level-up occurred since the last call to clearLevelUp().
    bool leveledUp() const { return leveledUp_; }
    void clearLevelUp() { leveledUp_ = false; }

    /// @brief Add XP directly (also called by onEvent).
    void addXP(uint32_t amount);

private:
    ExperienceManager();

    /// @brief Persist current stats to /int/xp_stats.json.
    void save();

    /// @brief Load stats from /int/xp_stats.json.
    void load();

    /// @brief Check if XP exceeds the threshold and level up.
    void checkLevelUp();

    /// Path on internal flash for the stats JSON.
    static constexpr const char *STATS_PATH = "/int/xp_stats.json";

    /// Maximum achievable level.
    static constexpr uint16_t MAX_LEVEL = 999U;

    /// XP required for level N: 100 * N  (level 1→100, level 2→200, …)
    static constexpr uint32_t xpThreshold(uint16_t lvl) { return 100U * static_cast<uint32_t>(lvl); }

    uint16_t level_;
    uint32_t xp_;
    uint32_t hackPoints_;
    bool leveledUp_;
    bool initialized_;
};
