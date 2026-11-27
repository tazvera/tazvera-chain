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

# RWA registry, part 1.5 — a recovery carries the assets

## Fixed behaviour (decided by the owner of the project)

- When an agreed recovery comes due, **every** asset whose owner is the old address gets the new address as owner, in the same step as the balance. There is no "chosen assets" option (more complex, and it opens a way to lose an asset).
- The registry keeps its size and each record keeps its kind, doc_hash and created_at; only `owner` changes. Assets of other owners are not touched. The state root changes because the registry changed.
- `RecoveryEvent` has a new field `asset_count`: how many assets were carried over.

## Decisions taken without being asked (please confirm or change)

1. **All or nothing with the balance.** If the new address cannot hold the balance (it would overflow), the recovery ends with nothing moved, as before, and the assets stay with the old address too (`moved == false`, `asset_count == 0`). Assets do not overflow, but splitting the step would leave funds and assets under different addresses.
2. **An account with assets but no money** is carried over too (balance 0, assets move).
3. **`asset_id` does not change** when the owner changes. It was derived from the first owner and nonce and stays the asset's identity; it does not name the current owner.
4. **Cost:** a recovery walks the whole registry once (O(registry size)) per recovery that comes due. Fine for a prototype; an index by owner is a later change if the registry gets large.
5. **Not shown in the wallet statement.** `wallet_statement` still shows balances only; a statement line for "assets moved" was not asked for, so `src/export.cpp` / `export.hpp` are unchanged.
6. **The old key is still alive** after recovery (its guardians are retired, as before). If a thief later registers a new asset with the old key, it belongs to the old address; recovery runs once per guardian agreement.

## Files

`src/persist.cpp` is **not** touched (not needed: only the owner of existing records changes, and the chain file is rebuilt by replay). `CMakeLists.txt` has one new test target only.

# RWA registry, part 2 — transfer the ownership of an asset (TransferAsset)

## Fixed behaviour (decided by the owner of the project)

- New transaction kind `TransferAsset` (7). It names the asset (asset_id), the new owner (`to`) and the current owner (`owner`), and is signed by the current owner's key, i.e. the owner the registry holds now.
- Refused: asset not in the registry ("asset was not found"); `owner` is not the registry's owner ("only the owner may transfer this asset"); wrong signature; `to` empty; fee other than the transfer fee.
- Only the owner changes. asset_id, kind, doc_hash and created_at stay.
- The same asset cannot be sold twice: after the first transfer the old owner is no longer the owner, so a second transaction for it (same block or later) is refused, and a block that contains one is refused as a whole.
- **While a recovery is pending for the owner's address, the asset cannot be moved** ("asset transfer is blocked while a recovery is pending"). A thief with the stolen key would try exactly that during the guardians' delay.
- **The protocol version stays 3 (devnet). A new transaction kind means all nodes must upgrade at the same time; before the chain is public the protocol must become 4** (this is the same open item as for kind 6; it is now two kinds).

## Decisions taken without being asked (please confirm or change)

1. **The asset id travels in the existing `ref` field** (the 32-byte field that CancelHeld already uses for a txid). No new field was added, so the chain file needs no new key. `to` and `owner` are the existing fields too.
2. **"Recovery pending" means a recovery that has been agreed and is waiting out its delay** (`recovery.active`). Guardian votes that have not yet reached the threshold do **not** block a transfer. Reason: one guardian could otherwise freeze the owner's assets just by voting (votes expire only after VOTE_EXPIRY_BLOCKS and can be re-cast). The cost: between the first vote and the threshold a thief can still move the asset. If the opposite trade-off is wanted (block while any vote is open), it is a one-line change in `State::apply`, with a test.
3. **Sending an asset to yourself is refused** (`to == owner`), as a plain transfer to yourself is.
4. **An asset can be sent to any non-zero address**, including one that has never been seen on the chain. There is no "accept" step, so a wrong address loses the asset (the transfer is final). Same as money.
5. **The receiver pays nothing and signs nothing.** Only the sender's fee is burned.
6. **The registry check comes after the usual checks** (nonce, balance for the fee). So a stranger with no money gets "insufficient balance", not "only the owner may transfer this asset".
7. **`owner`, `asset_kind` and `doc_hash` fields on other kinds stay refused**; `owner` is now allowed on TransferAsset too (and must be the signer); `asset_kind` and `doc_hash` must be zero there.
8. **File format:** a transfer is written in the same layout as a registration (with `owner`, `asset_kind` = 0 and `doc_hash` = zeros), so `src/persist.cpp` and `src/export.cpp` needed a one-line change each (the loader accepts kind 7; the writer writes the three keys for kind 7). Format level stays v4.

## Not done here, on purpose

- No `taznode` command, no explorer display, no wallet-statement line for an asset transfer (the fee is counted in the statement's total; there is no "asset sent" entry).
- No sale, price, escrow or "swap asset for money": a transfer of an asset and a transfer of money are two separate transactions.
- No allowance/approval (someone else moving your asset for you).
- No limit on how often an asset can change hands.

# taznode: RWA commands (`--register-asset`, `--transfer-asset`)

## Fixed behaviour (decided by the owner of the project)

- `--register-asset <kind>:<doc_hash_hex>` registers an asset owned by the key's address; `--transfer-asset <asset_id_hex>:<to_address>` gives an asset to another address.
- Both need `--chain`, `--expect-genesis` and `--key-file`, make exactly one block and stop, like `--transfer`.
- kind must be 1..4 and the hash 64 hex characters (asset id 64 hex, address 40 hex); anything else is a usage error (exit 2) and nothing is written.

## Decisions taken without being asked (please confirm or change)

1. **One action per run.** `--transfer`, `--register-asset` and `--transfer-asset` cannot be combined with each other, with a second copy of themselves, with `--listen`/`--serve`/`--peer` or with `--new-key` (usage error). Same rule as `--transfer`.
2. **`--register-asset` prints the new asset id** (`taznode: registered asset id=<64 hex>`), because the id is made from chain id, owner and nonce and the user needs it for `--transfer-asset`. Nothing else was added to the output; the existing "produced block ..." line is unchanged. There is no command that lists a chain's assets yet.
3. **An upper-case document hash is accepted** and stored as the same 32 bytes (the chain file writes lower case).
4. **The chain decides, not the command:** an asset that does not exist, or that the key does not own, is refused by the chain (the block is not made, exit 5, the file is not touched), not by a usage check. The command does not look up the registry first.
5. **Exit codes follow `--transfer`:** 2 usage, 3 cannot write, 5 the block was refused.
6. **The same arguments again** (`--register-asset 1:<same hash>`) make a new asset each time (the id includes the nonce). A repeated command therefore registers a second asset. This is the chain's rule from part 1, not something the command can prevent.
7. **No `--allow-anonymous`-style or producer options** were added: on a chain that declares producers the block is signed by the key, exactly as `--transfer` does.

# Network layer: a Transport between the messages and the socket

## Fixed behaviour (decided by the owner of the project)

- `net.hpp` has an interface `Transport` (`send_all`, `recv_exact`) and `PlainTransport`, which does on a `Socket` exactly what the message layer used to do itself. The wire does not change by a single byte.
- `send_message`, `recv_message`, `exchange_hello` and `exchange_authenticated_hello` take a `Transport&`. `taznode` and the tests call them through a `PlainTransport`.
- This change adds a layer only: no new kind of transport, no new option, no change in what a node does.
- The proof that behaviour is the same: the old tests pass with their assertions unchanged. In `test_net.cpp` and `test_hardening91.cpp` only the plumbing changed (a `PlainTransport x_t(x);` line per socket, and `x_t` in place of `x` as the first argument of the four functions); with those lines and the `_t` removed, both files are identical to what they were.

## Decisions taken without being asked (please confirm or change)

1. **`Transport` has two members more than `send_all` / `recv_exact`:** `prepare_send(deadline, error)` and `shutdown()`.
   - `shutdown()`: when the peer breaks the protocol (an announced length over the ceiling, a chunk stream out of order) the message layer cuts the connection. It did that with `::shutdown` on the socket; the layer can no longer touch a socket, so the transport does it.
   - `prepare_send()`: before the first byte of a message the old code turned the exchange deadline into the socket's timeout, once per message (not once per frame). To keep that exact behaviour the layer calls `prepare_send` once per message and `PlainTransport` does what the old code did. Default for any other transport: nothing to do.
2. **The raw `send_all(const Socket&, ...)` and `recv_exact(const Socket&, ...)` stay** as free functions. They are what `PlainTransport` is made of, and the HTTP API and the tests that write deliberately broken frames use them. The `Socket` overloads of the four message functions are **removed** (not kept as wrappers), so no path can reach the message layer without a `Transport`.
3. **`PlainTransport` does not own the socket** (it holds a reference): the caller closes the socket, as before, and keeps it alive as long as the transport.
4. **`PlainTransport::send_all` ignores its `deadline` argument.** A send was never bounded by the deadline except through the socket timeout that `prepare_send` sets, and that stays so.
5. **The deadline is checked once per message before the first byte** (the old behaviour), although a comment in the old code said "every chunk". Making it per frame would be a behaviour change and was not done here.

## Not done here, on purpose

- No second transport and no command-line option for one.
- `api.cpp` (the HTTP API) is not on the message layer and is unchanged.

# taznode: `--listen-on <address>`

## Fixed behaviour (decided by the owner of the project)

- `--listen-on <address>` sets the IPv4 address that `--listen` and `--serve` wait on. Default 127.0.0.1, so nothing that exists today changes. A wrong address is a usage error (exit 2) and nothing is written.
- `net.cpp` no longer has `INADDR_LOOPBACK` written into the listening function: `listen_on(address, port, ...)` takes the address, and `listen_loopback(port, ...)` is now `listen_on("127.0.0.1", ...)` (the tests read the bound address back to prove that it can never be anything else).

## Decisions taken without being asked (please confirm or change)

1. **0.0.0.0 is accepted** ("every interface"). Without it a node could only be opened to the network by naming one interface's address, which is awkward on a test network. The previous comment in `net.hpp` ("no way to ask for 0.0.0.0") is replaced.
2. **Only dotted-quad IPv4 is accepted** (four decimal parts, 0..255, no leading zeros). No host names ("localhost"), no IPv6, no short forms like `127.1`. The check is our own, not `inet_pton`, so it means the same on Linux and Windows.
3. **`--listen-on` is refused unless `--listen` or `--serve` is given**, also with `--peer`, `--api`, `--transfer` and the other actions, and with `--new-key`. It is refused when given twice.
4. **A warning on stderr when the address is not 127.x:** "listening on <address> makes this node reachable beyond this machine; the connection is not encrypted, so consider --identity with --require-identity". The node still starts: refusing to start without `--require-identity` would be a policy nobody asked for. If you want that policy, it is a one-line check.
5. **The "listening on ..." line now names the address** (`listening on 0.0.0.0:<port>`). For the default it is the same line as before (`listening on 127.0.0.1:<port>`), which the existing tests wait for.
6. **`--peer` still connects to 127.0.0.1 only.** A second machine can now connect *to* this node, but this node cannot connect out to another machine. A `--connect-to <address>` is the natural next flag; it was not asked for.
7. **The HTTP API (`--api`) stays on 127.0.0.1** and is not affected.
8. **Plain text on the wire.** `--listen-on` does not add encryption. The authenticated handshake (`--identity`, `--require-identity`, `--allow-peer`) proves who the peer is; it does not hide the traffic.

---

# `taznode --produce-every <seconds>`

## Fixed behaviour (decided by the owner of the project)

- With `--serve`, the node makes a block every N seconds (N from 1 to 3600). Empty blocks are fine.
- The timestamp stays strictly greater than the previous block's: each block gets `max(now, tip + 1)`.
- With `--key-file`, the blocks are signed with that key.

## Decisions taken without being asked (please confirm or change)

1. **One thread.** The node alternates between waiting for a peer and making a block: the wait for a peer ends at the next block time. A peer exchange that runs long delays the next block; blocks are not made in a burst afterwards (the next block is N seconds after the last one was made). Block times therefore drift a little; this is a test network clock, not a schedule.
2. **The tip is read again at every tick**, because a peer may have brought blocks in between. If the tip's timestamp is `now + 120` or more, the node writes a warning ("beyond this clock") and skips that tick instead of exiting. It waits for the clock to catch up.
3. **A producing node does not leave after 5 peers.** Without `--max-peers`, `--serve --produce-every` serves peers until `--serve-timeout` (or a signal) stops it. An explicit `--max-peers` still counts, as before.
4. **Chains that declare producers** need `--key-file` with a declared producer's key; it is checked before the port is opened (usage error, exit 2). The blocks are made with `append_block_signed`. With several producers it is not always this node's turn: a refused tick is written to stderr and skipped; it is not fatal.
5. **Chains that declare no producers** get unsigned blocks. `--key-file` is accepted and ignored (a note on stderr says so).
6. **Usage errors (exit 2):** `--produce-every` without `--serve` (so also with `--listen`, `--peer`, `--api`, `--transfer`, `--register-asset`, `--transfer-asset`, `--new-key`), given twice, or outside 1..3600.
7. **A block that cannot be saved** stops the node with exit 3 (memory and disk would otherwise disagree). A refused or skipped tick does not.
8. **Only empty blocks.** There is no mempool in taznode, so nothing waits to be put into these blocks. Transactions still go in with `--transfer` and the other one-shot commands.
9. **The key file is read once at start.** A node that makes blocks for days keeps the key in memory. Nothing new is written about key storage here.
10. **Two nodes producing on the same chain** are a fork race, as they would be anywhere without consensus. This flag does not try to prevent that; it is for one producing node plus followers (`--peer --every`).


# TLS transport shutdown behavior

- `TlsContext::Conn` exposes TLS read/write and peer-certificate fingerprint operations, but no TLS shutdown/close-notify operation.
- `TlsTransport::shutdown()` currently shuts down the underlying socket and releases the TLS connection so the message layer can cut off a broken peer. This does not send a TLS `close_notify` alert.
- Decision needed: should `TlsContext::Conn` expose a bounded, error-reporting TLS shutdown/close-notify operation, and should `TlsTransport::shutdown()` attempt it before shutting down the socket? Keep that behavior change in a separate change to the TLS foundation.
