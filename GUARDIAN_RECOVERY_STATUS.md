# Guardian recovery — what the chain really does today

**Read this first.** The three design papers (`GUARDIAN_RECOVERY_DESIGN.md`, `..._IMPLEMENTATION_BOUNDARIES.md`,
`..._TEST_PLAN.md`) describe a **key-rotation** model (stable account ID, 2-of-3, finalize transaction) and say
"not implemented". The chain, since the v6–v8.2 work, already implements a **different** recovery: **fund migration**.
Both are written down here so nobody builds the second one by accident on top of the first.

## What is implemented (code: `src/state.cpp`, `src/tx.cpp`; tests: `tests/test_recovery.cpp`, `tests/test_security82.cpp`)

| Piece | Rule in the code |
|---|---|
| Guardians | `SetGuardians`: up to `MAX_GUARDIANS` = 5, sorted and unique, none is the account itself, threshold between 1 and the list size |
| Threshold | M-of-N chosen by the owner (the papers fix 2-of-3) |
| Vote | `RecoveryVote` by a guardian names the account and the **new address**; votes for different targets do not add up |
| Agreement | when `threshold` guardians name the same address, a recovery is **pending** for `RECOVERY_DELAY_BLOCKS` = 300 |
| Execution | at the ready height the **whole spendable balance** moves to the new address and the old account's guardians are cleared; if the target cannot hold it, nothing moves |
| Owner cancel | `RecoveryCancel` by the account's own key (only when no veto key is set); this one cancels an agreed recovery completely |
| Veto key | optional separate key; once set, the account's own key may no longer cancel, only the veto key acts. Before agreement it clears the votes; on an **agreed** recovery it can only **postpone once** by `VETO_POSTPONE_BLOCKS` = 300 (a second veto fails), so a thief holding the veto cannot block forever (R2) |
| Changing guardians | any change of an existing list waits `SAFETY_DELAY_BLOCKS` = 500; a change that comes due while a recovery is agreed is **discarded** |
| Votes | cleared when the guardian list changes, when a recovery is cancelled, and when it runs |
| Canonical state | guardians, veto, votes, pending recovery and the `veto_postponed` byte are in the state root |

## What the papers ask for and the chain does NOT have

1. **Stable account ID and key rotation.** Not implemented, and not needed for fund migration. The honest cost of the
   current model: the recovered funds sit at a **new address**, and **anything sent to the old address later stays
   with the old key** (a thief's key if it was stolen). Recipients must be told the new address.
2. ~~Request expiry.~~ **Done in v9.3 (protocol 3):** a vote counts for `VOTE_EXPIRY_BLOCKS` = 1000 blocks and is then
   dropped; its height is in the state root. (The papers' separate expiry height on the whole request is not needed
   for fund migration: the 300-block delay already bounds an agreed recovery.)
3. **Proof of possession of the veto key.** The veto is just an address; the chain cannot check that anyone holds it.
   A veto address nobody controls means nobody can cancel.
4. **Fixed 3 guardians / 2-of-3.** The code allows 1–5 with any threshold.
5. **Separate finalize transaction.** Recovery runs automatically at the ready height in `begin_block`.

## Honest limits (do not hide these in any wallet text)

- Any `threshold` guardians acting together can move the balance after the delay; the owner's protection during the
  delay is the owner key (no veto set) or the veto key.
- Funds that are held (waiting inside held transfers) are not touched by a recovery.
- Prototype only: no real funds, no external security review.

## Suggested next steps, smallest first

1. ~~Vote expiry~~ done in v9.3.
2. Decide on purpose whether key rotation is wanted at all. It is a protocol migration (see the design paper), much larger
   than fund migration, and fund migration already protects the main case (lost or stolen key).
3. Only then touch the papers: bring their wording in line with the model that is chosen.
