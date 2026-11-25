# TAZVERA Public Consensus Design — Draft v1

**Status: design only. Not implemented. Not approved for testnet or production.**

## 1. Goal

Replace the prototype's deterministic fork tie-break and fixed producer round-robin with a real multi-validator consensus protocol. Independent nodes must agree on one ordered block history, and a committed block must not be reverted by a longer competing chain.

This document is a design proposal. It does not change the current block or transaction protocol.

## 2. Proposed protocol

Use a Tendermint-style Byzantine Fault Tolerant (BFT) proof-of-stake consensus:

- Validators have Ed25519 consensus keys and a voting power derived from bonded stake.
- Each height proceeds through rounds. A deterministic proposer is selected from the active validator set, weighted by voting power.
- Validators exchange signed PROPOSAL, PREVOTE and PRECOMMIT messages.
- A block is committed only after more than two-thirds of the active voting power has precommitted the same block hash for the same chain ID, height and round.
- A validator follows a lock/unlock rule across rounds; it must never precommit conflicting blocks at the same height. The precise state machine and proof rules must be specified before implementation.
- If the network cannot reach the threshold, the height remains uncommitted and round timeouts advance. There is no longest-chain or smallest-hash fallback.
- A commit certificate is stored and independently verified by every syncing node.

## 3. Fault assumptions and safety

The intended BFT safety assumption is that strictly less than one-third of active voting power is Byzantine. Liveness requires a sufficiently connected and responsive supermajority. Network partitions may stop progress; the protocol must prefer safety over committing conflicting histories.

A validator that signs conflicting votes must create objectively verifiable equivocation evidence. Slashing policy and exact penalties are not yet specified and must not be improvised in code.

## 4. Validator-set bootstrapping

For the first public testnet, use an explicitly published, fixed genesis validator set with independently operated keys and documented voting power. This is only a controlled bootstrap, not permissionless decentralization.

Dynamic validator registration, bonding/unbonding, stake accounting, reward distribution, slashing, validator-set updates and governance require a separate economic and state-transition specification. They must be agreed before enabling open validator entry.

## 5. Required protocol changes

The current prototype is not sufficient as-is:

- The current producer list is a fixed round-robin schedule, not stake-weighted BFT consensus.
- The current fork choice selects the longer chain and uses a tip-hash tie-break at equal height. This must never decide finality in the public protocol.
- The current block producer signature is not a commit certificate. Consensus votes and certificates need canonical, domain-separated encodings.
- Consensus-critical fields (chain ID, protocol version, height, round, step, validator-set/epoch commitment, block hash and vote target) must be covered by signatures.
- Block/header and persistence formats need a deliberate protocol-version migration. Old prototype chain files must not be silently treated as public-network history.
- Peer authentication currently proves control of a key, not that the key is an authorized validator. Validator authorization and encrypted transport are separate requirements.

## 6. Implementation stages

1. **Formal state machine:** specify message formats, round transitions, proposer selection, locking/unlocking, timeout behavior, commit-certificate verification and restart recovery.
2. **Deterministic simulator:** simulate validators, message delay, reordering, loss, partitions, crashes and Byzantine equivocation without sockets.
3. **Consensus core:** implement the state machine and cryptographic vote verification independently of networking.
4. **Adversarial tests:** prove no two conflicting blocks can both obtain valid commit certificates under the stated fault bound; test stale votes, duplicate votes, wrong chain/height/round, malformed certificates, restart and validator-set changes.
5. **Network integration:** authenticated validator authorization, encrypted transport, bounded message handling, peer limits and safe sync.
6. **Private multi-node testnet:** independent operators and keys, no real-value assets.
7. **Public testnet:** only after independent review, operational monitoring, recovery procedures and security assessment.

## 7. Mandatory test cases

- 4 validators with 1 Byzantine validator: progress and safety under a healthy network.
- 4 validators with 2 validators offline or partitioned: no conflicting commit; liveness may stop.
- Two conflicting proposals at the same height and round.
- Equivocating proposer and equivocating voter.
- Delayed, duplicated, reordered and replayed messages.
- Votes from the wrong chain ID, height, round, step or validator set.
- Insufficient voting power, exactly two-thirds, and strictly greater than two-thirds.
- Validator restart while locked; persisted lock must not be forgotten.
- Validator-set transition at an epoch boundary.
- Conflicting commit certificates must be rejected and treated as a critical safety failure.
- Deterministic replay: all honest nodes compute identical application state and state root after each committed block.

## 8. Decisions required before implementation

- Initial validator count and independently controlled operators.
- Stake unit, voting-power calculation, minimum/maximum stake and concentration limits.
- Block time and timeout schedule.
- Epoch length and validator-set update rules.
- Rewards, fees, issuance and slashing economics.
- Whether the first testnet uses fixed genesis voting power or a fully implemented staking module.
- Protocol upgrade and emergency halt/recovery governance.

## 9. Non-goals for this draft

This draft does not claim the chain is ready for public testnet, does not define a production token economy, does not authorize real funds, and does not replace an independent security audit.


## 4A. Validator ON/OFF and stake unlock rules

These are two separate operations and must never be treated as the same switch.

### Validator status: ON / OFF

- **ON (Active):** the validator is eligible for proposer selection and may PREVOTE/PRECOMMIT, provided its key and validator-set entry are valid.
- **OFF (Inactive):** the validator voluntarily stops participating. It is excluded from the active voting-power denominator only through a deterministic, height-bound validator-set update; nodes must not locally remove it immediately.
- An OFF request is signed by the validator's authorized consensus key (or submitted through the defined governance/staking transaction), has a nonce and target epoch/height, and is replay-protected.
- The change takes effect only at a finalized epoch boundary after the required validator-set update is committed. Until then, the validator follows the currently committed set.
- Reactivating an OFF validator requires a new valid ON request and takes effect only at a later finalized epoch boundary. The validator must pass key/eligibility checks again.
- If an operator simply goes offline, the network must not silently rewrite the validator set. It remains in the committed set until the protocol's scheduled update rule applies.

### Stake unlock / unbonding

- **Unlock is not OFF.** Turning a validator OFF does not release its bonded stake.
- A separate signed UNBOND request starts a protocol-defined unbonding delay. The stake remains bonded and unavailable for transfer during that delay.
- Stake becomes withdrawable only after the unbonding period has elapsed, all applicable pending penalties/evidence windows have been checked, and a finalized state transition marks it withdrawable.
- No instant withdrawal, administrator-only unlock, or manual database edit is permitted.
- Exact unbonding duration, evidence window, minimum stake, and penalty rules are unresolved parameters. They must be fixed in a separate staking-economics specification before implementation; do not invent values in code.

### Safety requirements

- Validator-set changes must be deterministic and committed as part of consensus state.
- The voting-power denominator for a height is the validator set committed for that height; all honest nodes must calculate the same threshold.
- Tests must cover OFF/ON requests, replayed requests, requests during a partition, epoch-boundary activation, validator restart, unbonding delay, pending equivocation evidence, and attempts at early withdrawal.
- This section is a design requirement only. It does not implement validator controls or enable staking withdrawals.


## 10. Simulator coverage added (transport only)

The educational network simulator models deterministic message delay, duplicate
delivery, FIFO/LIFO reordering, message drops across configured partitions, and
partition healing. These tests validate the transport fault harness only.
They do **not** demonstrate BFT safety or liveness: validator locks,
cryptographic signatures, voting-power quorum, equivocation evidence,
persistent restart recovery, and conflicting-commit proofs remain unimplemented.
