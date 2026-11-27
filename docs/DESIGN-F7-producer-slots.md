# F7 — Producer slot skipping (design only)

## Current rule in main

Reviewed against main at `8bb8fa2`.

The producer list is committed in order to the genesis commitment. `Chain::append_block_signed` selects the producer for the next height as `producers_[height() % producers_.size()]`; verification in `Chain::verify` expects `producers_[(block.height - 1) % producers_.size()]`. Thus height 1 uses producer 0, height 2 producer 1, etc. The block carries a timestamp, but no slot number. A missing producer cannot be skipped: the next height still expects the same producer.

## Proposed slot rule

Do not infer a skipped slot from the number of blocks: no block means no height advance, so that rule cannot recover from a silent producer.

Define a protocol-level `slot_duration` and `genesis_time`, and a monotonically increasing **slot number** derived from the block timestamp. A candidate block must:
- have a slot strictly greater than the parent block's slot;
- be timestamped inside that slot's allowed interval (including a specified bounded clock drift);
- be signed by `producers[(slot - 1) % producer_count]`;
- reference the exact parent tip and pass normal transaction/state validation.

**Skipping threshold:** advance to the next slot only after the current slot's deadline plus the protocol's fixed grace interval. The slot index itself advances by one per elapsed slot; if multiple slots elapsed, the candidate may use the latest eligible slot, and the intervening producer slots are explicitly skipped. Set the exact duration and grace as protocol constants before implementation; do not silently choose a local wall-clock timeout. All nodes must derive eligibility from the block's committed timestamp/slot, not their local arrival time.

This permits progress when a producer is offline, but it does not guarantee progress if enough producers are offline or clocks/timestamps cannot satisfy the rule.

## Two blocks for the same slot

A producer schedule alone cannot prevent two competing signed blocks for one slot. Specify a deterministic fork-choice rule before coding:

- A block is eligible only if its slot and parent are valid and its producer signature is valid.
- If two eligible blocks claim the same parent and slot, compare their block hashes lexicographically; the lower hash is the deterministic provisional winner.
- Nodes must not finalize either branch merely because it arrived first. If descendants already exist, apply the documented fork-choice rule consistently across the competing branches and reorg only through the existing validated-chain mechanism.
- Document the security limitation: a hash tie-break gives deterministic agreement, not Byzantine finality or protection from a producer that can grind many valid blocks.

If the implementation cannot safely reorg competing same-slot branches, reject the second branch and make the liveness/safety tradeoff explicit rather than claiming a complete tie-break.

## Protocol compatibility and rollout

**Yes—this changes consensus/protocol validity.** Existing blocks have no slot field and use height-based producer selection. The new rule changes which producer is valid and when a block is valid. It must be gated by an explicit protocol version/activation height or a new chain genesis; all nodes validating that chain must upgrade together before activation. Mixed old/new validators can disagree on valid blocks and split the network. Do not activate it as a silent implementation detail or retroactively reinterpret old blocks.

## Tests required before implementation is accepted

- Current round-robin vectors remain unchanged before activation.
- Slot 1/2 schedule and wraparound across the producer list.
- Current producer offline: candidate at the next eligible slot accepted; early skip rejected.
- Multi-slot gap: exact eligible slot/producer is selected; skipped slots cannot be filled late.
- Timestamp at slot boundary, just before/after boundary, grace expiry, overflow, and invalid clock-drift cases.
- Wrong producer, wrong slot, non-increasing slot, and slot inconsistent with timestamp are rejected without state mutation.
- Two valid same-parent/same-slot blocks produce the same deterministic winner regardless of arrival/order.
- Different parent, different slot, and descendants of competing blocks exercise deterministic fork choice and reorg.
- Restart/export/import preserves slot metadata and makes the same schedule/fork choice.
- Activation boundary: old rule before activation, new rule at/after activation; mixed-version behavior fails closed or is explicitly rejected.
- Regression for genesis pin, block hash/signature, state root, and legacy chain compatibility.
- Property/fuzz tests over slot arithmetic, producer-list lengths, timestamp extremes, and competing branch orderings.
