#include "taz/fork.hpp"
#include <algorithm>

namespace taz {

ForkDecision choose_chain(const Chain& mine, const Chain& theirs) {
    if (mine.blocks().empty() || theirs.blocks().empty())
        return {ForkChoice::KeepMine, "one of the chains is empty"};
    if (mine.genesis_commitment() != theirs.genesis_commitment())
        return {ForkChoice::DifferentChain, "different genesis commitment"};

    if (theirs.height() > mine.height())
        return {ForkChoice::TakeTheirs, "the peer's chain is longer"};
    if (theirs.height() < mine.height())
        return {ForkChoice::KeepMine, "our chain is longer"};

    // The tip hash, not the state root: two different block histories can end at the same state (two unrelated
    // transfers in a different order, for instance), and calling those "the same chain" would leave both nodes
    // on different histories while each believed they agreed.
    const Hash32 mine_tip = mine.blocks().back().hash();
    const Hash32 theirs_tip = theirs.blocks().back().hash();
    if (mine_tip == theirs_tip) return {ForkChoice::SameChain, "same height and same tip block"};

    // Same height, same genesis, different history: two chains of the same age that disagree. Pick one by a rule
    // both sides can compute, so they do not drift apart. The smaller tip hash wins; the choice carries no
    // security meaning, only agreement.
    const bool theirs_wins = std::lexicographical_compare(theirs_tip.begin(), theirs_tip.end(),
                                                          mine_tip.begin(), mine_tip.end());
    return {theirs_wins ? ForkChoice::TakeTheirs : ForkChoice::KeepMine,
            theirs_wins ? "same height, the peer's tip sorts first" : "same height, our tip sorts first"};
}

}  // namespace taz
