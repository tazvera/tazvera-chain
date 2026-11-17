# Core v9 — From a Library to a Chain: the Three-Node Devnet

**Status:** design proposal. Nothing here is implemented. No branch exists yet, no protocol constant has changed, and no test result is claimed.
**Baseline:** `main` = `c233b7a` plus the `core-v8.7-integration-repair` branch (`7ec7cb9`), which puts the v8.7 fixes into `src/`, `include/taz/` and `explorer/`.
**Goal of this milestone:** three separate machines produce and follow **one** chain, and a node that was switched off for a while can rejoin and reach the same state.

---

## 0. Why this is the right next stone

Everything in the repository today is a *library*: one process, one chain, saved to a file. It is careful work, and it is verified on Linux, Windows and clang — but nothing yet makes two computers agree, so nothing yet makes it a chain.

The next milestone is deliberately the smallest thing that changes that:

> Block 0 is fixed. A known list of producers exists. Each height has exactly one allowed producer. Every block carries that producer's signature. Three nodes gossip blocks over TCP and end up with the same state root.

That is not consensus in the research sense, and it does not survive a malicious majority. It is the *plumbing* on which a real consensus algorithm can later sit, and it is small enough that one person can finish and test it.

What it buys you: producer authentication, a wire protocol, a node binary, a multi-node test harness, and the first real answer to "does the code agree with itself on two machines?"

What it does not buy: safety under Byzantine faults, liveness under partition, finality guarantees, staking, slashing. Those come after, and §7 says how.

---

## 1. What must change in the block (protocol 3)

Today a block is `{protocol_version, chain_id, height, previous_hash, timestamp, transactions, state_root}` and `hash()` commits to all of it. Nothing says **who** produced it, so any peer can extend the chain.

### 1.1 The header

```cpp
struct Block {
    std::uint32_t protocol_version{PROTOCOL_VERSION};
    std::string   chain_id{CHAIN_ID};
    std::uint64_t height{0};
    Hash32        previous_hash{};
    std::int64_t  timestamp{0};
    Address       producer{};            // v9: who made this block
    std::vector<Transaction> transactions;
    Hash32        state_root{};
    Signature     producer_signature{};  // v9: Ed25519 over the block hash; NOT part of the hash
    Hash32 hash() const;                 // commits to every field above except the signature
};
```

Rules:

- `producer` is inside the hash, so it is part of the block's identity and cannot be swapped after the fact.
- `producer_signature` is **outside** the hash (a signature over a value that contains the signature is circular). It signs `"TAZVERA-BLOCK-SIGNATURE-V1" || hash()`, domain-separated exactly like transactions.
- The genesis block keeps `producer = 0` and a zero signature: block 0 is defined by the genesis manifest, not by a producer.

### 1.2 The producer set is part of genesis

The devnet needs a fixed list of who may produce. Put it in the genesis manifest, and commit it:

```
genesis_manifest = {
    protocol_version, chain_id, params,
    accounts:  [ {address, balance}, ... ],
    producers: [ address, ... ],      // v9: sorted, unique, 1..N
}
genesis_commitment = SHA-256("TAZVERA-GENESIS-V3" || ...manifest... || genesis_block_hash)
```

This is the payoff of the v8.2 work: the producer list becomes as authenticated as the balances. Changing who may produce changes the commitment, which changes block 0, which breaks every later hash — so nobody can quietly add themselves.

### 1.3 Proposer selection

For the devnet: **strict round-robin over the sorted producer list**, `proposer(height) = producers[height % producers.size()]`, with `height` starting at 1 (block 0 has no producer).

- Deterministic, trivially testable, no randomness needed.
- A validator must reject a block whose `producer` is not `proposer(height)` — even a correctly signed one. This single rule is what stops one node from producing every block.
- Rotation means one missing producer stalls the chain. That is acceptable for a devnet and is exactly the problem a real consensus algorithm solves later (§7).

### 1.4 Version and file format

Adding a field to the header changes every hash, so this **is** a protocol change and must be labelled as one:

- `PROTOCOL_VERSION = 3`.
- Export format `tazvera-chain-export-v5`; a v1/v2/v3/v4 file is refused with a message that says so (the loader already has this pattern).
- `TAZVERA-BLOCK-V1` becomes `TAZVERA-BLOCK-V2` in `Block::hash()`, so an old block can never be mistaken for a new one.
- Everything else — transaction bytes, state roots, safety rules, guardian recovery — stays byte-identical. v8.2's rules and v8.7's fixes carry over unchanged; only the header and the genesis manifest grow.

**Migration:** no real chain files exist, so there is nothing to migrate. Regenerate the three demo files with the new exporter, as in v8.1.

---

## 2. The wire protocol (minimal, on purpose)

One TCP connection, length-prefixed frames, no discovery in v9.

### 2.1 Frame

```
u32 length (big endian, max 4 MiB)  ||  u8 type  ||  payload
```

### 2.2 Handshake (first frame each way)

```
HELLO { u32 protocol_version, str chain_id, Hash32 genesis_commitment, u64 best_height, Address me }
```

A peer is dropped immediately if the protocol version, chain ID or genesis commitment differs. That one check prevents the most common devnet failure — two nodes happily talking past each other on different chains.

### 2.3 Message types

| Type | Payload | Direction | Notes |
|---|---|---|---|
| `GET_BLOCKS` | `u64 from_height, u32 max` | either | sync request |
| `BLOCKS` | `u32 count, [block...]` | either | response, ≤ `MAX_BLOCKS_PER_MESSAGE` |
| `NEW_BLOCK` | `block` | producer → all | gossip; the receiver validates before accepting |
| `NEW_TX` | `transaction` | any → all | gossip into the mempool |
| `PING` / `PONG` | `u64 nonce` | either | liveness, used for peer scoring |

Every received block goes through the *same* path as a locally produced one: `Chain::accept_block` with the v9 rules. A block that arrives twice is dropped by hash. A block whose height is not `tip + 1` triggers a `GET_BLOCKS` sync rather than an accept.

### 2.4 Limits that must exist from day one

- Max frame 4 MiB; reject and disconnect beyond it, before allocating.
- Max 32 peers; max 8 outbound connections.
- Per-peer token bucket: e.g. 100 messages/second, 4 MiB/second.
- A peer that sends an invalid block, an invalid signature or an unserialisable frame is disconnected and its address is refused for 10 minutes.
- One thread per peer, one shared `std::mutex` around the chain, or a single-threaded event loop with `poll()`. **Recommendation: one thread per peer + a chain mutex.** It is the least clever option and the easiest to reason about at this size.

### 2.5 Serialisation

Do **not** reuse the JSON file format on the wire. Use a compact binary encoding (fixed-width fields, the same layout discipline as the state root) with the same strict rules: exact lengths, no trailing bytes, and a maximum size before allocation. The JSON parser stays for files and the explorer.

---

## 3. The node binary

```
taznode --genesis genesis.json --key node.key --listen 0.0.0.0:7001 \
        --peer 10.0.0.2:7001 --peer 10.0.0.3:7001 \
        --max-timestamp-drift 5
```

- `node.key` — an Ed25519 seed file, mode 0600, never printed, wiped on exit.
- On start: load the genesis file, check the genesis commitment against `--expect-genesis` if given, then sync to the best peer before producing anything.
- Produces a block when `proposer(next_height) == me`, on a timer (devnet: 5 s), including mempool transactions, signed with the node key.
- `SIGTERM` → stop producing, flush the chain file, close peers cleanly.
- Logs: one line per accepted block (`height`, `producer`, `txs`, `root`), one per rejected block with the reason. These logs are the devnet's only diagnostic tool, so make them boring and complete.

No RPC in v9. The node's job is to produce and follow a chain; the API layer is the milestone after.

---

## 4. Fork handling, stated honestly

With a fixed proposer per height, two blocks at the same height only appear after a partition. The devnet rule:

1. Prefer the chain with the greater height.
2. At equal height, prefer the smaller block hash (deterministic, no clocks).
3. A reorg deeper than `MAX_REORG_DEPTH` (say 50) stops the node and logs an error instead of silently following — a devnet should fail loudly, not drift.

This is a documented devnet compromise, not a consensus algorithm. Write it in the README exactly like that.

---

## 5. Tests — the milestone is only real if these pass

### 5.1 In-process (fast, runs on every CI job)

- `test_block_producer_signature`: a block with a wrong producer, a missing signature, a signature by the wrong key, or a producer that is not `proposer(height)` is refused.
- `test_proposer_rotation`: heights 1..10 across three producers select exactly the expected producer each time.
- `test_genesis_commits_producer_set`: editing the producer list in a file changes the commitment and is refused.
- `test_wire_roundtrip`: encode/decode every message type, plus truncated, oversized and trailing-byte frames are refused.
- `test_wire_handshake_mismatch`: a different chain ID, protocol version or genesis commitment is refused before any block is exchanged.

### 5.2 Multi-process (the actual milestone)

A harness that spawns three `taznode` processes on `127.0.0.1` with distinct ports and keys:

- `test_devnet_three_nodes_agree`: after 30 blocks, all three report the same height and the same state root.
- `test_devnet_rejoin`: stop node C, let A and B advance 20 blocks, restart C — C syncs to the same root without replaying from a file.
- `test_devnet_partition`: block the port of B for 10 blocks, then reopen it — the chain converges and B logs one reorg, not a silent divergence.
- `test_devnet_bad_block_rejected`: a fourth node that signs a block with a key not in the producer set is disconnected and its block is refused by all three.
- `test_devnet_restart_keeps_state`: kill all three, restart all three, they resume at the same height.

The harness must be part of `ctest`, not a manual script, and it must run in the Linux CI job. Windows can keep running the in-process tests only.

### 5.3 Exit criteria for the milestone

All of the following, on real CI, with the logs pasted into the PR:

1. Three nodes, three processes, 30+ blocks, identical height and root.
2. A killed node rejoins and matches without human help.
3. A partition converges, and the reorg is logged.
4. A non-producer's block is refused by every node.
5. `ctest` green on Linux Release, Linux ASan+UBSan, Windows, and the fuzz smoke job.
6. The README says plainly what this is not (§7).

---

## 6. What deliberately does **not** change

Everything below stays exactly as it is in v8.7, and the devnet tests must keep proving it:

- Transaction bytes, `txid`, all signature domains for transactions.
- State root encoding (V1/V2/V3) and the supply invariant.
- Safety rules (daily limit, holds, cancel) and guardian recovery with the veto.
- The persistence layer: atomic save, tamper detection, genesis commitment and the out-of-band pin.
- The offline explorer and its own re-implementation of the rules.

That is the point of building the network layer separately: if the devnet breaks a safety rule, the bug is in the new code, not in a rewritten state machine.

---

## 7. After the devnet: the honest order of work

1. **Finality.** Round-robin has none. Adopt a published BFT algorithm (CometBFT-style two-phase commit, or HotStuff) — do not invent one. That is where "safety under Byzantine faults" first appears.
2. **Validator economics.** Fee split, supply cap, minimum stake, unbonding, slashing. Your README already says these are undecided; they are needed before any public testnet.
3. **RPC + light client.** Merkle root in the header, proofs for balances and inclusion, a JSON-RPC endpoint that is not on the validator port.
4. **Wallet.** HD keys, QR addresses, and a recovery screen — your differentiator is invisible without it.
5. **Genesis ceremony and operations.** Reproducible genesis from a published file, seed nodes in two providers, monitoring, runbooks, a rehearsed fork drill.
6. **Audit.** External, before real value. Non-negotiable.

If at step 1 you find that a published consensus engine fits better than your own, that is not a defeat — it is the decision the plan document asked you to keep open.

---

## 8. India, honestly

A chain is not Indian because it says so; it is Indian if it works for Indian users: fast settlement on cheap hardware, a fee model people can afford, recovery UX that matches how families actually hold money, and compliance you can show a regulator (FIU-IND registration, 1% TDS reporting, 30% VDA tax handling). None of that is a slogan you can put in a README — each one is work, and most of it is on this list.

The devnet is the first stone that makes any of it possible to talk about seriously, because until two machines agree you cannot honestly say "chain" at all.

---

## 9. Proposed first commit

Small, self-contained, reviewable on its own:

> `feat(block): producer field and signature in the block header (protocol 3)`

- `Block` gains `producer` and `producer_signature`; `hash()` covers the producer and the new domain string.
- `Chain::init_genesis` takes the producer list; the genesis commitment covers it.
- `Chain::verify`/`accept_block` check the producer, the signature and the rotation.
- Export/import updated; format label `-v5`; demo files regenerated.
- The in-process tests from §5.1.
- The node, the wire protocol and the multi-process harness follow in separate commits.

Say the word and I will write that commit the same way as before: design in the PR body, code with its tests, and real CI output before anything is called ready.
