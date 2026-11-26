# TAZVERA Consensus State Machine v1 — Engineering Specification Draft

**Status: draft for review. This document is not executable consensus code and does not authorize a public testnet.**

## 1. Scope and non-goals

This specification defines the minimum safety contract for a weighted, round-based BFT consensus core. It does not define token issuance, rewards, slashing amounts, staking economics, governance, validator admission economics, block-time targets, or production networking. Those remain separate decisions.

The first implementation target is a deterministic C++26 state machine that can be tested without sockets. Network integration is a later milestone. The existing round-robin producer schedule, longest-chain selection, and tip-hash tie-break are prototype behavior only; none may determine public-network finality.

## 2. Fixed inputs for one height

At height H, every honest node must use the same finalized inputs:

- chain ID and protocol version;
- previous committed block hash and application state root;
- validator-set commitment for H;
- each validator's unique consensus public key and positive integer voting power;
- the total voting power W, computed with checked arithmetic.

The validator set for H is immutable during H. A validator-set change is accepted only through a separately finalized state transition and applies at the explicitly committed activation height/epoch. Nodes must never locally remove an offline validator from the denominator.

The quorum threshold is the smallest integer strictly greater than two-thirds of W: floor(2*W/3)+1. Implementations must calculate it without overflow and reject a set whose total voting power cannot be represented safely. Votes are counted once per validator, not once per message.

## 3. Canonical vote identity and signatures

Every consensus vote has exactly these signed fields:

- domain tag TAZVERA-CONSENSUS-VOTE-V1;
- chain ID;
- protocol version;
- height;
- round;
- step: PREVOTE or PRECOMMIT;
- target: a 32-byte block hash or the explicit NIL target;
- validator-set commitment;
- validator consensus public key.

Encoding must be canonical, length-delimited or fixed-width, endian-defined, and versioned. Signatures must cover the exact canonical bytes. A signature from another chain, protocol version, height, round, step, target, or validator set must fail verification. NIL is a distinct target, never an all-zero block hash alias.

A vote is accepted only if its key belongs to the committed validator set and its signature verifies. Unknown keys, duplicate votes, malformed encodings, and conflicting votes from one key are rejected from quorum accounting. Conflicting signed votes are retained as equivocation evidence; they do not count twice.

## 4. Round steps and transitions

Each height starts at round 0. Each round has PROPOSAL, PREVOTE, and PRECOMMIT steps.

1. **PROPOSAL:** the deterministic proposer for (height, round, validator-set commitment) may propose one block. The proposer schedule must be deterministic, weighted by voting power, and independently testable. A proposal includes its height, round, block, proposer identity, and any valid-round proof it relies on.
2. **PREVOTE:** an eligible validator prevotes either a valid proposal block or NIL. A validator must not prevote a block that conflicts with its persisted lock unless the proposal carries a valid-round proof that satisfies the unlock rule in section 5.
3. **PRECOMMIT:** after observing strictly more than two-thirds prevoting power for a block in the current round, a validator locks that block and precommits it. After strictly more than two-thirds prevoting power for NIL, it precommits NIL. Otherwise it waits for the round timeout; it must not invent a vote merely to advance.
4. **COMMIT:** a block is committed only after strictly more than two-thirds of the height's voting power has produced valid PRECOMMIT votes for the same block hash at the same height and round. The resulting commit certificate contains the canonical votes or a lossless, independently verifiable aggregate representation. A node validates the full certificate before applying the block.
5. **NEXT ROUND:** if no block commits, the round advances only according to deterministic timeout/step rules. Votes from earlier rounds never contribute to a later-round quorum.

A node must reject a certificate that mixes heights, rounds, steps, validator-set commitments, chain IDs, or block hashes. A longer chain or a smaller hash is never a substitute for a commit certificate.

## 5. Lock safety and restart behavior

Lock state is consensus-critical persistent state. It includes at least the locked block hash and locked round, plus enough verified proof material to justify any later unlock.

- A validator persists its new lock atomically before it emits the corresponding PRECOMMIT vote.
- A restart must restore the lock before the validator can sign any vote at that height.
- A validator must never precommit two different block hashes at the same height, even across rounds or process restarts.
- Unlocking is allowed only when a later-round proposal includes a verifiable PREVOTE certificate for that same block at a round strictly greater than the validator's locked round, and the validator applies the specified safe relocking transition before voting.
- If required lock data is missing, corrupt, or cannot be authenticated, the node must fail closed for consensus signing at that height. It may still serve read-only status and request state repair.
- Local clock changes, process restarts, message order, and network reconnection must not erase or weaken a lock.

The exact proof object and transition code must be covered by executable state-machine tests before networking is enabled.

## 6. Equivocation evidence

For one validator, two distinct valid signed votes with the same chain ID, validator-set commitment, height, round, and step but different targets constitute objective equivocation evidence.

Evidence verification must be deterministic and must not depend on the reporter. Duplicate copies of the same evidence are idempotent. Evidence from different rounds or different steps is not automatically equivocation. Penalties and evidence-window lengths are intentionally unspecified here; no slashing behavior may be implemented until the economic and governance rules are approved.

## 7. Validator lifecycle boundary

ON/OFF and stake unbonding are separate state transitions.

- An ON/OFF request is scheduled and takes effect only at a finalized activation boundary encoded in consensus state.
- An offline validator remains in the committed validator set until that transition takes effect.
- OFF does not release stake.
- Unbonding starts only through a separately authorized state transition. Stake remains unavailable until the protocol-defined delay and evidence window have both elapsed and a finalized withdrawal transition is committed.
- Exact delay, evidence window, minimum stake, stake-to-voting-power conversion, concentration cap, rewards, and penalties remain unresolved parameters. They must not be guessed in code.

## 8. Determinism and failure policy

All consensus-critical calculations must be deterministic across supported platforms. Avoid floating-point arithmetic. Use checked integer arithmetic for voting power, thresholds, proposer selection, and height/round counters. Reject overflow, invalid validator sets, duplicate consensus keys, zero voting power, malformed blocks, and unsupported protocol versions.

Safety takes priority over liveness. During a partition or when quorum is unavailable, the node may stop committing. It must never lower the quorum, silently alter the validator set, or fall back to longest-chain selection.

## 9. Required implementation gates

The state-machine core is not ready to integrate with the node until all of the following exist:

- canonical encoders/decoders and signature test vectors;
- weighted quorum boundary tests: below, exactly two-thirds, and strictly above two-thirds;
- duplicate, unknown, stale, replayed, wrong-chain, wrong-height, wrong-round, wrong-step, and wrong-set vote tests;
- conflicting proposals and equivocation evidence tests;
- lock/unlock tests across multiple rounds;
- crash/restart tests proving locks survive before PRECOMMIT signing;
- commit-certificate validation tests, including mixed-field and insufficient-power certificates;
- deterministic replay tests on Linux and Windows;
- adversarial simulation for delay, duplication, reordering, message loss, partitions, crashes, and recovery;
- an independent review of the safety argument before network integration.

Only after these gates pass should the consensus core be connected to authenticated validator networking. Public testnet additionally requires independent operators, monitoring, recovery procedures, and external security review.

## 10. Open decisions that must not be invented

The following need explicit project approval before implementation: initial validator count and operator independence; stake denomination and conversion to voting power; block/round timeout parameters; epoch length; activation rules; unbonding/evidence windows; rewards, fees, issuance and slashing; governance and emergency recovery; protocol upgrade rules; and whether the first private testnet uses a fixed genesis validator set before permissionless validator admission is enabled.
