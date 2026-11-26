# F5 — Open decisions and explicit scope

## Fixed behavior for this PR

- When two peers have the same genesis and height but disagree on history, the current implementation can only test whether the peer's advertised tip attaches directly to the local chain after removing the local tip.
- If that attachment fails, the node must reject the sync, emit a clear refusal message, and return a non-zero result. It must not report the peer as successfully served or the sync round as successful.
- This PR does not attempt a common-ancestor search or a multi-block reorganization.

## Deferred decisions

1. **Common-ancestor reorganization:** a separate PR must define and test how to locate the highest common ancestor, validate every replacement block in order, and commit the replacement atomically. Until then, deeper forks are refused.
2. **Fork choice and finality:** the existing height/tip-hash selection rule is only a deterministic tie-break, not consensus or finality. It must not be described as consensus.
3. **Peer retry policy:** whether a node should automatically retry a refused deep fork, and with what backoff/limit, remains a separate networking decision. This PR only guarantees that the failed sync is not reported as success.
4. **Unequal-height divergent histories:** this PR's regression tests cover equal-height deep forks only. A separate test/review must define behavior when a longer peer chain diverges before the local tip; no common-ancestor reorg is introduced here.

## Validation boundary

- Regression cases: divergence at block 1, with both chains at heights 2, 3, and 10.
- Expected result: non-zero exit from the connecting node, explicit "unrepaired deep fork" refusal, and neither chain file modified by the failed exchange.
- No changes to networking protocol, consensus, persistence format, or common-ancestor reorganization.


# F11 — Genesis supply bound

## Fixed behavior for this PR

- Genesis creation rejects when the sum of the two genesis balances exceeds `std::numeric_limits<Amount>::max()` (the uint64 maximum).
- The check uses subtraction (`second_balance > max - first_balance`) so the validation itself cannot overflow.
- The check runs before any chain or producer state is mutated, for both producer-free and producer-enabled genesis creation.
- A total supply exactly equal to uint64 maximum remains valid.
- The existing receiver-overflow regression now uses a legal maximum-supply genesis; the state-level overflow guard remains defense in depth.

## Deferred decisions

1. **Protocol supply cap:** this PR treats uint64 maximum as the upper bound. Whether TAZVERA should adopt a lower economic/protocol supply cap is a separate protocol decision.
2. **Genesis account count:** the current API accepts exactly two genesis accounts. If genesis later supports a variable number of allocations, the same checked-sum invariant must cover every allocation before committing any state.
3. **Commitment helper semantics:** `genesis_commitment_of(...)` remains a deterministic hash helper, not a validation API. Callers that create a chain must use the checked `Chain::init_genesis` path; whether to expose a separate validated genesis builder is deferred.

## Validation boundary

- Overflow by one (uint64 max + 1) is rejected for both genesis overloads.
- Exact uint64 maximum is accepted for both overloads.
- Rejection leaves chain state and producer configuration unchanged and permits a subsequent valid genesis.


# Explorer — deferred until the RWA phase

The following Explorer issues are confirmed by code review and intentionally remain open.
They are not part of F10; address them together with the planned RWA-era Explorer redesign.

1. **Chain identity validation:** Explorer does not enforce the expected chain_id against the protocol's fixed chain identity. A file from a different chain identity may be displayed as if it belongs to this chain.
2. **Duplicate genesis address edge case:** the duplicate-address check can be bypassed when the first genesis account has zero balance. The Explorer must reject duplicate addresses regardless of either balance.
3. **Producer signature verification:** Explorer does not independently verify producer signatures. The RWA-era redesign must either implement verification consistent with the C++ core or clearly refuse to present producer authenticity as verified.

**Scope decision:** no Explorer code changes in F10. These three items are deferred to the RWA phase, when the Explorer will be redesigned.


# F10 — Persistent writer lock

## Fixed behavior for this PR

- Writers acquire an exclusive OS lock on a separate, permanent sidecar file named <chain-path>.lock before creating temporary output or renaming the chain file.
- The sidecar lock file is never deleted, renamed, or swept as a temporary file. This prevents writers from locking different inodes after replacement.
- Readers do not acquire the writer lock. They continue reading the previous complete chain while the writer prepares a new file and atomically renames it.
- POSIX uses an exclusive advisory file lock. Windows uses LockFileEx with LOCKFILE_FAIL_IMMEDIATELY; a competing writer is refused without waiting.
- Lock acquisition failures include an explicit warning that NFS/SMB and other network filesystems may not provide reliable locking; use a local filesystem for shared chain files.
- The OS releases the lock when a process is forcibly terminated. The permanent sidecar remains in place.

## Validation boundary

- Two independent processes contend for the same sidecar: only one writer may hold it.
- A reader can load the old complete file while another process holds the writer lock.
- A competing writer is refused promptly with the network-filesystem warning.
- A child process is forcibly terminated (SIGKILL on POSIX; TerminateProcess on Windows); a later writer must acquire the lock successfully.
- On POSIX, the sidecar inode is checked before and after the crash/recovery cycle to prove it was not replaced.
- Concurrent in-process writers retry transient lock contention and every successful save remains a complete valid chain.
- No reader lock, chain-file format change, network protocol change, or Explorer code change is included.

# RWA registry, part 1 — register an asset (no transfer yet)

## Fixed behaviour in this change

- New transaction kind `RegisterAsset` (6) with `owner` (20 bytes), `asset_kind` (1 land, 2 vehicle, 3 invoice, 4 other) and `doc_hash` (32 bytes). Only the owner may register, in the owner's own name; the signer must be the owner.
- `asset_id = SHA-256("TAZVERA-ASSET-V1" || chain_id || owner || u64be(nonce))`, all 32 bytes. The domain length (16) is `static_assert`ed. The id is pinned in a test to digests computed with another SHA-256 implementation.
- The registry (`asset_id -> {owner, asset_kind, doc_hash, created_at}`) is part of the state and of the state root: a non-empty registry switches the root to a new domain `TAZVERA-STATE-V4` (V3 layout plus the registry). A chain that never registers anything keeps its V1/V2/V3 root exactly as before.
- The fee is the transfer fee (`FEE_TRANSFER_V1`, burned). Nothing else moves.
- Only a registration carries the three new fields in the chain file; every other transaction is written exactly as before.

## Protocol version

- **The protocol version stays 3 (devnet).** Adding a transaction kind means that every node must upgrade at the same moment: a node that does not know kind 6 refuses a block that contains one, and the chain splits. **Before the chain is public the protocol must become 4.**

## Decisions taken without being asked (please confirm or change)

1. **`chain_id` in the asset id is hashed as its raw bytes, with no length prefix.** The brief writes `|| chain_id ||`; the signed transaction bytes use a length prefix. `chain_id` is a fixed constant of the chain, so the two are equally unambiguous today. Changing this later changes every asset id.
2. **A zero `doc_hash` is accepted.** The brief did not say. It is almost certainly a mistake by the registrant, but rejecting it is a rule nobody asked for.
3. **The duplicate-id check in `State` is defence in depth and has no test of its own.** `asset_id` contains the owner's nonce, and a nonce is used once, so a repeated registration is refused earlier, as a bad nonce. The test for "the same registration twice" therefore proves the nonce path; the registry check cannot be reached through `apply`.
4. **File format:** a registration is only readable in the layout that carries the veto key (format v4 and later, which is what the file label becomes for any kind of transaction 3 and above). It is refused in v1/v2 layouts.
5. **`asset_kind` is a closed set 1..4.** A fifth kind needs a rule change (and, once public, a protocol bump).

## Not done here, on purpose (separate changes)

- **Transfer of ownership** of an asset. Until it exists the registry is write-once.
- **Guardian recovery does not follow assets.** An asset is keyed by the owner's address at registration; when a key is recovered to a new address the registry still names the old one. How recovery and the registry interact is the next design question (it is the purpose of step 3 of the plan).
- **No `taznode` command** to register an asset yet; the tests build transactions directly.
- **The explorer (JavaScript) does not understand kind 6.** The existing demo chains contain none, so the cross-check is unaffected; showing assets is a later change.
- **No limit on the size of the registry.** The fee is the only cost; the state root is recomputed over all of it. The loader's existing size limits (and finding F2 of the audit) apply to the chain file.
- **Documents:** only a hash is stored. Keeping personal or confidential documents off the chain is a rule for whoever writes the application, not something the chain can enforce.
