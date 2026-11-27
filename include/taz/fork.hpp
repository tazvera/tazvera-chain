#pragma once
#include "taz/chain.hpp"

namespace taz {

// v9.0: which of two chains should a node keep.
//
// This is a deterministic tie-break, NOT consensus. Two nodes that see the same two chains will agree on which
// one to keep, which is what stops them drifting apart silently. It does not stop a determined adversary, it
// has no finality, and it is not safe for money. Calling it consensus would be a lie, and the honest name for
// what it does is "pick one, the same way every time".
//
// The rule, in order:
//   1. a different genesis commitment is never a choice — the two chains are unrelated (refused);
//   2. the longer chain wins;
//   3. at equal height, the smaller tip hash wins (an arbitrary but fixed and verifiable tie-break);
//   4. identical chains are the same chain.
enum class ForkChoice { SameChain, KeepMine, TakeTheirs, DifferentChain };

struct ForkDecision {
    ForkChoice choice{ForkChoice::SameChain};
    std::string reason;
};

ForkDecision choose_chain(const Chain& mine, const Chain& theirs);

}  // namespace taz
