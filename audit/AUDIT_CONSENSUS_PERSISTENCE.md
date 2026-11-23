# TAZVERA Chain — audit of consensus, fork resolution, block validation and persistence

Base: `main` @ `cc00464` (core 9.3, protocol 3). Branch: `claude/audit-consensus-persistence`.
Method: read every line of `src/chain.cpp`, `src/state.cpp`, `src/tx.cpp`, `src/fork.cpp`, `src/block.cpp`, `src/persist.cpp`,
`src/crypto.cpp`, `include/taz/*.hpp`, and the sync / fork / production parts of `tools/taznode/main.cpp`; then reproduced
what could be reproduced with `audit/repro_audit.cpp` (library harness) and the real `taznode` binary.
Out of scope (as asked): `src/net.cpp`, `src/hello.cpp`, `src/auth.cpp` (handshake and transport). `src/mempool.cpp` was not reviewed.
**No production file was changed.** Nothing was merged.

Severity scale: **High** = breaks availability or correctness for honest users without special privileges;
**Medium** = needs a privileged role (producer) or a specific situation, or is a cost amplification; **Low/Info** = hardening.
"Verified" = reproduced by running code. "Code reading" = I did not run it.

## Summary

| # | Severity | Finding | Status |
|---|---|---|---|
| F1 | High | A node more than 1000 blocks behind (or a new node on a chain older than 1000 blocks) can never sync | Verified (process) |
| F2 | High | A chain the node saved itself becomes unloadable (about 117,000 blocks, 2 M JSON values; also 200,000 blocks / 64 MiB); empty blocks are free | Verified |
| F3 | High | Transactions are not bound to a chain's genesis: a tx signed on one TAZVERA network replays on any other network that has the same address and nonce | Verified |
| F4 | Medium | Any peer can force an O(chain) re-verification per connection (fork path); invalid 10,000-tx block costs ~1 s | Verified (library) + code reading |
| F5 | Medium | A fork two or more blocks deep never heals, and the node reports the failed comparison as success | Verified (library) + code reading |
| F6 | Medium | Equal-height tie-break is grindable: the scheduled producer can erase its own just-published block | Verified |
| F7 | Medium | Round-robin has no slot skipping: one offline producer stops the chain for ever; a producer can also stall the next one up to 120 s | Code reading |
| F8 | Medium | A chain with no producer set has no block authority (anyone may extend the tip) | Code reading |
| F9 | Medium/Low | Guardian recovery does not move held (in-flight) funds; the retired key can still cancel its held transfers | Code reading (not reproduced) |
| F10 | Low | Two `taznode` processes on one chain file: last writer wins, no lock | Code reading |
| F11 | Low | Genesis supply is unbounded; `State::circulating()` throws on a legal genesis | Verified (library API only) |
| F12 | Low/Info | Whole chain rewritten on every block (O(n²) I/O); hold threshold is bypassed by splitting; others (see below) | Code reading |

Checked and **not** found (negative results): long-range chain replacement (see "What I tried that did not work"), replay of an old
block, a block with a wrong chain ID / protocol / height / previous hash / producer / signature, partial state change from a rejected block,
height wrap, double-spend inside one block, nonce overflow, amount+fee overflow, receiver-balance overflow, crash between write and rename.

---

## F1 — A node cannot sync when it is more than 1000 blocks behind  (High, verified)

* `tools/taznode/main.cpp:595-601` — `kMaxSyncAhead = 1000`; if `theirs.best_height > chain.height() + 1000` the peer is refused ("refused before syncing"). `main.cpp:353,365,393` — at most 200 blocks per round.
* Consequence: a **new node** (height 0) can never join a chain longer than 1000 blocks, and a node that was off for more than 1000 blocks can never catch up. The refusal is permanent: the gap only grows.
* Reproduce (verified, output below):
  ```
  g++ -std=c++20 -O2 -Iinclude audit/repro_audit.cpp build/libtazcore.a -lcrypto -lpthread -o repro_audit
  PIN=$(./repro_audit files /tmp/syncdir 1101)         # full.json = 1101 blocks, genesis.json = same genesis, no blocks
  build/taznode --chain /tmp/syncdir/full.json    --expect-genesis $PIN --serve 47311 --max-peers 1 --serve-timeout 15 &
  build/taznode --chain /tmp/syncdir/genesis.json --expect-genesis $PIN --peer 47311 ; echo rc=$?
  ```
  Output: `taznode: the peer claims to be 1101 blocks ahead; more than 1000 is refused before syncing`, `rc=3`.
* Direction: keep a per-connection bound, not a per-gap refusal: sync in rounds of ≤200 and continue while each round makes progress.

## F2 — The node can write a chain file it cannot read back  (High, verified)

* `include/taz/persist.hpp:29-31` and `src/persist.cpp:122,359,452,476`: loader limits are 64 MiB, 2,000,000 JSON values, 200,000 blocks. `save_chain_json` (`persist.cpp:489`) has no matching check.
* Empty blocks are legal and free (an unsigned chain lets anyone append; with producers, the scheduled producer can). Measured: 130,000 empty blocks build in under 1 s, save "OK" at 53 MiB, and the load of that same file is **refused: "the file holds too many JSON values"**. Threshold measured between 110,000 (loads) and 120,000 (refused) blocks. At one block per 10 s that is ~13 days.
* After a restart the node cannot start (it must load with a pin and cannot); every other node hits the same wall. A 64 MiB file is also reachable with ~58,000 ordinary transactions (each costs 1000 units).
* Reproduce: `BLOCKS=120000 ./repro_audit persist /tmp/p`.
* Also `main.cpp:450,850`: the **whole file is rewritten and fsynced on every block** (O(n²) total I/O).
* Direction: make save and load limits one constant (or refuse to produce/accept a block that would exceed it); minimum block interval or fee for empty blocks; incremental storage.

## F3 — No cross-chain replay protection  (High for your multi-chain use, verified)

* `src/tx.cpp:21-26, 30-37`: a transaction signs `protocol_version`, `chain_id`, from/to/amount/fee/nonce/expiry/pubkey. `CHAIN_ID` is a single constant (`include/taz/types.hpp:400`) shared by **every** TAZVERA prototype network. The genesis commitment (which differs per network) is not signed.
* So a transaction that Alice signed for network A is valid on network B if B has the same address with the same nonce and enough balance. Chain files are public and contain every signed transaction, so anyone can collect them.
* Reproduce (verified): `./repro_audit replay` → chain A accepts the tx, chain B accepts the **same bytes** and credits Bob 400000 that Alice never authorised there.
* **Practical consequence for you:** if the new protocol-3 chain reuses an address from an old chain (for example `0ead4f09…`), every transaction ever signed by that address on the old chain can be replayed, in nonce order, on the new one. **Use brand-new keys for both genesis accounts.**
* Direction (needs a protocol bump): put the genesis commitment (or a per-network ID derived from it) into the transaction signing domain and the block-signing domain.

## F4 — CPU amplification: one cheap connection costs a full-chain replay  (Medium)

* `tools/taznode/main.cpp:657-668`: when a peer's hello says "same height, different tip", the node copies the chain and calls `candidate.truncate_to(height-1)` **before** looking at anything the peer sent beyond a parseable block. `src/chain.cpp:109-125` rebuilds from genesis and re-verifies every signature.
* Measured (library): 40,000 transactions → `truncate_to` = **4.0 s**; it grows linearly with chain size. A failed attempt does not count against `--max-peers`, so it can be repeated.
* Related: `src/chain.cpp:100-102`: a 10,000-transaction block whose last transaction is invalid costs **~1.0 s** (`./repro_audit cost`). On a chain without producers anyone can send such a block at tip+1. On a producer chain the signature check (`chain.cpp:91-98`) runs first, so only the scheduled producer reaches it.
* I did not run the full process-level amplification; the path is straight-line code (`main.cpp:607-668`).
* Direction: validate the peer's tip header (height, previous hash vs our block h-1, producer and signature) before copying or truncating; cache a snapshot instead of rebuilding from genesis; rate-limit per address.

## F5 — A fork deeper than one block never heals; failure is reported as success  (Medium)

* `main.cpp:593-691` only resolves "same height, different tip", assuming the chains share block h-1. `receive_blocks` (`main.cpp:430`) can only attach on top of our own tip.
* Reproduce (verified): `./repro_audit split` — A has g,a1,a2; B has g,b1. `B.accept_block(a2)` → "previous hash does not match chain tip". `choose_chain` says "TakeTheirs" but the protocol only ever sends block 2.
* If the peer's tip does not attach (`main.cpp:664-668`), the node logs "keeping our chain" and **returns true** (as it does at `main.cpp:640-644`, 673-675), so the exit code is 0 and the peer counts as served while the two nodes stay on different chains. Operators and `--max-peers` cannot see the split.
* Direction: return a distinct "diverged" result; add a find-common-ancestor step and an explicit maximum reorg depth.

## F6 — Tie-break is grindable; the producer can erase its own tip  (Medium, verified)

* `src/fork.cpp:25-31`: at equal height the smaller tip hash wins. The hash covers the timestamp (`src/block.cpp`), which the producer chooses.
* Reproduce (verified): `./repro_audit grind` — producer publishes block 1 containing a payment to a merchant, then mints a rival block 1 without the payment; it found a rival with a smaller hash in **3 tries**; a node holding the first block that meets a node holding the rival **takes the rival** and the payment is gone.
* Window: until the next block is built on the tip (afterwards the rival is shorter). So one confirmation is worthless and two are the minimum. Equivocation leaves no evidence the chain can use. On a producer-less chain anyone can do this.
* Direction: document finality as ≥2 blocks now; later, equivocation proofs (two signatures at one height) and a rule that a producer that equivocates is skipped or slashed.

## F7 — Liveness: no slot skipping, timestamp stalling  (Medium, code reading)

* `src/chain.cpp:91`: block h must be signed by `producers_[(h-1) % n]`. If that producer is offline or withholds, **no block is valid** and nobody else may fill the slot; the chain stops permanently.
* `src/chain.cpp:36-40` with `main.cpp:814-824`: a producer may stamp its block `now+120`; the next producer then refuses to produce (`tip_time >= now+120`, `main.cpp:817`) until its clock catches up: up to 120 s per malicious turn.
* Direction: slot timeout and rotation; bound how far a timestamp may jump ahead of the previous one.

## F8 — A chain without a producer set has no block authority  (Medium, code reading)

* `src/chain.cpp:87-89`: with `producers_` empty, a block must simply leave the producer fields empty. Anyone can build the next block; the "winner" is decided only by F6's hash rule. Fine as a local prototype, not as a network. (`taznode` prints "unsigned: this chain declares no producers" at production time, `main.cpp:858`.)

## F9 — Recovery does not carry held funds  (Medium/Low, code reading, not reproduced)

* `src/state.cpp:94-113`: recovery moves `old.balance` only and clears the guardians. Transfers already in `held_` (the very feature that protects an account) are untouched; `CancelHeld` (`state.cpp:166-175`) is allowed for the old key afterwards and refunds into the **old** address. The old key needs 1000 units for the fee, which an accomplice can send.
* Direction: either document it, or make recovery also retarget or cancel the account's held transfers.

## F10 — Two processes on one chain file  (Low, code reading)

* `src/persist.cpp:489-585`: temp file names are unique per process, the rename is atomic, but there is no lock and no check that the file is still what was loaded. Two `taznode` processes (for example a `--produce` run while a `--serve` runs) overwrite each other's blocks: last writer wins.

## F11 — Unbounded genesis supply  (Low, verified for the library API)

* `src/chain.cpp:22-35`, loader `src/persist.cpp:~385-395`: genesis of 2^64-1 + 2^64-1 is accepted; `State::circulating()` (`state.cpp:38-45`) then throws `std::overflow_error`. Nothing in the node calls it, so impact is limited to library users. `./repro_audit overflow`.

## F12 — Other hardening notes (Low/Info)

* Hold threshold is bypassed by splitting a transfer below it (`state.cpp:142`); only the daily limit stops that.
* Block identity ignores who signed it (`include/taz/block.hpp`, intentional): fine while Ed25519 signatures are deterministic and OpenSSL rejects S ≥ L; add a test so it stays true.
* Block validity depends on each node's wall clock (`now+120`, `main.cpp:430`): nodes with skewed clocks can disagree about the same block.
* `verify()` copies the whole state per block (`chain.cpp:100`); `Chain candidate = chain` copies the whole chain (`main.cpp:659`). Memory spikes grow with accounts (an account costs 1000 units to create).
* A cycle of recoveries (A→B and B→A due in the same block) is deterministic (`state.cpp:94-113`, sorted by ready height and address) but order-dependent.

## What I tried that did not work (good news)

* **Long-range replacement by a longer chain from genesis.** The node only ever appends blocks that attach to its own tip (`main.cpp:430`) and refuses peers more than 1000 ahead, so reorg depth is exactly one block. (This is also why F1 and F5 exist.)
* Replaying an old block, a block with a wrong chain ID/protocol/height/previous hash, a block with producer fields on a producer-less chain, wrong producer, wrong pubkey, bad signature: all refused, in cheap-to-expensive order (`chain.cpp:81-102`).
* A rejected block leaves no partial state (`candidate` copy, `chain.cpp:100`).
* Transaction replay inside one chain: nonce is checked and incremented (`state.cpp:125`); the same txid held twice is refused (`state.cpp:144`).
* Crash during save: write → fsync → read-back → rename → fsync directory (`persist.cpp:489-585`); the loader replays everything from genesis and pins the genesis (`persist.cpp:321-445`).

## Missing tests and edge cases (proposed)

1. Sync: peer 999 / 1000 / 1001 blocks ahead; a new node joining a 5000-block chain (F1).
2. Persistence round-trip at the limits: N empty blocks, N blocks near 64 MiB; saving must never produce an unloadable file (F2).
3. Cross-chain: same transaction on two genesis commitments must be refused after the fix (F3).
4. Fork: depth 2, 3 and 10; peer's tip that does not attach must return a non-success result; same-height fork where the loser has the lower hash (F5, F6).
5. Cost bound: `truncate_to` and `validate_block` time on a 10,000-tx invalid block; tip header checked before truncation (F4).
6. Producers: scheduled producer offline; equivocation (two signed blocks at one height); timestamp `== prev+1`, `== max`, `max+1`, INT64_MAX; producer fields on a producer-less chain and the reverse.
7. Recovery with held transfers in flight; recovery chain/cycle in one block; vote expiry exactly at `VOTE_EXPIRY_BLOCKS` and +1 (`state.cpp:93`).
8. Genesis edge cases: both balances zero, balance sum ≥ 2^64, zero address, same address twice.
9. Two processes writing one file (F10); disk-full and read-only target during save.
10. Fuzzing: only `fuzz_load_chain` and `fuzz_tx` exist; add `parse_block_text` and a sync message sequence fuzzer.
11. Signature malleability: an Ed25519 signature with S+L must be refused (block and transaction).

## Reproduction index

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target tazcore taznode
g++ -std=c++20 -O2 -Iinclude audit/repro_audit.cpp build/libtazcore.a -lcrypto -lpthread -o repro_audit
./repro_audit replay      # F3
./repro_audit persist D   # F2   (BLOCKS=120000 to set the size)
./repro_audit cost        # F4
./repro_audit split       # F5
./repro_audit grind       # F6
./repro_audit overflow    # F11
./repro_audit files D 1101  # F1 helper, then the two taznode commands above
```
