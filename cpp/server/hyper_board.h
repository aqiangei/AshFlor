#pragma once
#include "server/db.h"
#include "shared/game/hyper_board.h"
#include <algorithm>
#include <limits>

namespace flix {
inline std::uint32_t hyperHoldingOwnerRank(const std::vector<HyperHoldingRow>& rows,
                                           const std::string& name) {
    const std::string* previous = nullptr;
    std::uint32_t rank = 0;
    for (const auto& row : rows) {
        if (!previous || *previous != row.name) ++rank;
        if (row.name == name) return rank;
        previous = &row.name;
    }
    return 0;
}

inline std::uint64_t apexHoldingCount(const PlayerRecord& record) {
    // The database normalizes saves to bag-only inventory on load.
    std::uint64_t total = 0;
    for (const auto& [key, value] : record.inventory[rarityName(Rarity::Ascendant)].members()) {
        if (key.rfind("petal_", 0) == 0)
            total += PlayerRecord::stackCount(value);
    }
    for (const auto& slot : record.loadout) {
        if (slot && slot->rarity == Rarity::Ascendant) ++total;
    }
    return total;
}

inline std::vector<HyperHoldingRow> apexHoldingBoard(const Database& db) {
    std::vector<HyperHoldingRow> rows;
    for (const auto& username : db.usernames()) {
        const auto* account = db.findUser(username);
        const auto* record = account ? db.findProgress(account->id) : nullptr;
        if (!record) continue;

        const auto total = apexHoldingCount(*record);
        if (total == 0) continue;

        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
        // An empty petal ID identifies an account total in the existing message layout.
        rows.push_back({username, "", count, count});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.ownerTotal != b.ownerTotal) return a.ownerTotal > b.ownerTotal;
        return a.name < b.name;
    });
    return rows;
}
} // namespace flix
