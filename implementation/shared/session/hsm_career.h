// A Progression fight's effect on the host's career money, in-session (the SaveGuard still restores the saved
// career when the session ends): a win pays the purse the suppressed win screen would have paid; a loss keeps the
// bet lost (stock behaviour) — and either way the pending Due Reward is spent, so it cannot pay out twice (#620).
#pragma once
#include <cstdint>

namespace hsm {

struct ProgPayout { int64_t fundsDelta = 0; int64_t dueAfter = 0; };
inline ProgPayout ProgPayoutFor(bool won, int64_t dueReward) {
    if (dueReward < 0) dueReward = 0;   // a corrupt negative purse is no purse: nothing is paid and nothing stays due
    return won ? ProgPayout{dueReward, 0} : ProgPayout{0, 0};
}

}  // namespace hsm
