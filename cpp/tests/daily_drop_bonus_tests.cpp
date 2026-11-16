#include "test.h"
#include "server/daily_drop_bonus.h"
using namespace flix;
TEST(daily_bonus_maintenance_pauses_until_next_login_without_manual_switch) {
    PlayerRecord r;const std::int64_t start=1700000000000;
    CHECK(claimDropBonus(r,start));
    auto& b=r.extra["dailyDropBonus"];
    const auto day=b["day"].asDouble();const auto streak=b["streak"].asInt();
    CHECK(pauseDropBonusForMaintenance(b,start+600000,"update-one"));
    CHECK_EQ(activeDropBonus(r,start+600000),1);
    CHECK(!pauseDropBonusForMaintenance(b,start+1200000,"update-one"));
    CHECK(resumeDropBonusAfterMaintenance(r,start+7200000));
    CHECK_EQ(b["until"].asDouble(),start+7200000+3000000);
    CHECK_EQ(b["day"].asDouble(),day);CHECK_EQ(b["streak"].asInt(),streak);
    CHECK_EQ(activeDropBonus(r,start+7200001),2);
    CHECK(!resumeDropBonusAfterMaintenance(r,start+7300000));
    CHECK_EQ(b["until"].asDouble(),start+7200000+3000000);
    CHECK_EQ(activeDropBonus(r,start+7200000+3000000),1);
    CHECK(!pauseDropBonusForMaintenance(b,start+7200000+3000000,"update-two"));
}

TEST(daily_bonus_pauses_without_losing_time_and_slow_ticks_spend_simulated_time) {
    PlayerRecord r;
    const std::int64_t start = 1700000000000;
    CHECK(claimDropBonus(r, start));
    CHECK(compensateSlowDropBonus(r, start + 100, 100, 50));
    CHECK_EQ(dropBonusRemaining(r, start + 100), 3599950.0);
    CHECK(setDropBonusEnabled(r, start + 100, false));
    CHECK(!setDropBonusEnabled(r, start + 200, false));
    CHECK_EQ(dropBonusRemaining(r, start + 1000), 3599950.0);
    CHECK_EQ(activeDropBonus(r, start + 1000), 1);
    CHECK(!compensateSlowDropBonus(r, start + 1000, 100, 50));
    CHECK(setDropBonusEnabled(r, start + 1000, true));
    CHECK(!setDropBonusEnabled(r, start + 1100, true));
    CHECK_EQ(activeDropBonus(r, start + 1000), 2);
    CHECK_EQ(dropBonusRemaining(r, start + 1100), 3599850.0);
    CHECK(!claimDropBonus(r, start + 1100));
}

TEST(daily_bonus_expires_at_beijing_midnight_and_caps_new_claim_at_one_hour) {
    const std::int64_t midnight = 20000LL * 86400000 - 8LL * 3600000;
    for (const bool paused : {false, true}) {
        PlayerRecord r;
        auto& bonus = r.extra["dailyDropBonus"];
        bonus["day"] = static_cast<double>(dropBonusDay(midnight) - 2);
        bonus["streak"] = 3;
        CHECK(claimDropBonus(r, midnight - 60000));
        CHECK_EQ(activeDropBonus(r, midnight - 60000), 3);
        CHECK_EQ(dropBonusResetAt(midnight - 1), midnight);
        if (paused) CHECK(setDropBonusEnabled(r, midnight - 30000, false));
        CHECK(!claimDropBonus(r, midnight - 1));
        CHECK(dropBonusRemaining(r, midnight - 1) > 0.0);
        CHECK_EQ(dropBonusRemaining(r, midnight), 0.0);
        CHECK_EQ(activeDropBonus(r, midnight), 1);
        CHECK(!setDropBonusEnabled(r, midnight, true));
        CHECK(!compensateSlowDropBonus(r, midnight, 100, 50));

        CHECK(claimDropBonus(r, midnight));
        CHECK_EQ(dropBonusRemaining(r, midnight), 3600000.0);
        CHECK_EQ(activeDropBonus(r, midnight), 2);
        CHECK_EQ(dropBonusStreak(r, midnight), 5);
        CHECK(!claimDropBonus(r, midnight + 1));
        CHECK(!claimDropBonus(r, midnight + 3600000));
    }
}

TEST(daily_bonus_maintenance_cannot_restore_previous_days_time) {
    const std::int64_t midnight = 20000LL * 86400000 - 8LL * 3600000;
    PlayerRecord r;
    CHECK(claimDropBonus(r, midnight - 60000));
    auto& bonus = r.extra["dailyDropBonus"];
    CHECK(pauseDropBonusForMaintenance(bonus, midnight - 30000, "update"));
    CHECK(resumeDropBonusAfterMaintenance(r, midnight + 30000));
    CHECK_EQ(dropBonusRemaining(r, midnight + 30000), 0.0);
    CHECK_EQ(activeDropBonus(r, midnight + 30000), 1);
    CHECK(!pauseDropBonusForMaintenance(bonus, midnight + 30000, "next"));
}
