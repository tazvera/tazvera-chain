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

    const Hash32 mine_root = mine.state().state_root();
    const Hash32 theirs_root = theirs.state().state_root();
    if (mine_root == theirs_root) return {ForkChoice::SameChain, "same height and same state root"};

    // Same height, same genesis, different state: two chains of the same age that disagree. Pick one by a rule
    // both sides can compute, so they do not drift apart. Smaller root wins; the choice itself carries no
    // security meaning, only agreement.
    const bool theirs_wins = std::lexicographical_compare(theirs_root.begin(), theirs_root.end(),
                                                         mine_root.begin(), mine_root.end());
    return {theirs_wins ? ForkChoice::TakeTheirs : ForkChoice::KeepMine,
            theirs_wins ? "same height, the peer's state root sorts first" : "same height, our state root sorts first"};
}

}  // namespace taz
