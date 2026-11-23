#pragma once
#include "server/db.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace flix {
inline double accountPlaytimeSeconds(const Database& db, const std::string& userId,
                                     const PlayerRecord& record) {
    const auto seconds = [](const Json& value) {
        const double number = value.asDouble();
        return std::isfinite(number) && number > 0 ? number : 0.0;
    };
    return seconds(db.storedTable("councilActivity")[userId]) +
        seconds(record.extra["councilActiveSeconds"]) +
        seconds(record.extra["inheritedPlaytimeSeconds"]);
}

struct PlaytimeRow { std::string name; double seconds; };
inline std::vector<PlaytimeRow> playtimeBoard(const Database& db, bool includeAdmins, std::size_t limit = 50) {
    std::vector<PlaytimeRow> rows;
    for (const auto& name : db.usernames()) {
        const auto* user = db.findUser(name);
        if (!user || user->guest || (user->admin && !includeAdmins)) continue;
        const auto* record = db.findProgress(user->id);
        if (!record) continue;
        const double seconds = accountPlaytimeSeconds(db, user->id, *record);
        if (std::isfinite(seconds)) rows.push_back({name, seconds});
    }
    const auto shown = std::min(limit, rows.size());
    std::partial_sort(rows.begin(), rows.begin() + shown, rows.end(), [](const auto& a, const auto& b) {
        return a.seconds != b.seconds ? a.seconds > b.seconds : a.name < b.name;
    });
    rows.resize(shown);
    return rows;
}
}
