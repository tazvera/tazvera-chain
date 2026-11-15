# TAZVERA Chain — Core v7

**Status: local prototype only. Not a testnet, not mainnet, and not the existing Shardeum TZV token. Do not use real funds or production/private wallet keys.**

This package replaces the broken Phase-1 core with a small, buildable C++20 foundation. It deliberately does **not** implement networking, validators, consensus, staking, smart contracts, a wallet, explorer, or persistence.

## What this package includes

- Real Ed25519 signing and verification through OpenSSL EVP (no placeholder `return true`).
- SHA-256 hashing through OpenSSL EVP.
- Transaction signatures cover protocol version, chain ID, sender, recipient, amount, fee, nonce, expiry, and public key.
- Sender address is checked against the transaction public key.
- Fixed prototype transfer fee of 1000 utzv; 100% burn in this local model. This is not a final or approved mainnet fee.
- Nonce, fee, balance, expiry, chain ID, signature, and integer overflow checks.
- Canonically ordered state map and deterministic state root.
- Block hash commits to block metadata, every transaction ID, and state root.
- Block execution uses a temporary state copy; if any transaction fails, no partial state is committed.
- CMake builds the library, demo, and tests; CTest runs the tests.

## Requirements

- C++20 compiler
- CMake 3.20 or newer
- OpenSSL 3.x development headers/libraries

Ubuntu/Debian example:

```bash
sudo apt update
sudo apt install build-essential cmake libssl-dev
```

## Build and test

Run from this folder:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/taz_demo
```

The test suite checks valid signatures, modified-signature rejection, wrong-chain rejection, transfer accounting, fee burn, replay/nonce rejection, atomic block failure, and block-hash commitment.

## Important limitations and next work

1. This is a repaired local core only. A successful local build is not proof of blockchain security.
2. No P2P networking, consensus, validator registration, staking, slashing, persistence, RPC, JSON protocol, wallet, or smart contracts are included.
3. The prototype uses one fixed transfer fee. Congestion policy, fee governance, validator economics, and the long-term sustainability of a finite staking reserve still need formal design.
4. The 100% fee-burn choice means transaction fees do not pay validators. Resolve validator rewards before public testnet/mainnet.
5. The address derivation here is a prototype convention, not a finalized public address standard.
6. Cryptographic code uses OpenSSL's Ed25519 implementation; do not invent custom cryptography.
7. Independent review, fuzzing, multi-node determinism tests, and a professional security audit are required before any real-value deployment.

## Existing project warning

Keep this independent-chain prototype separate from the existing Shardeum Mainnet TZV asset. This code does not bridge, migrate, wrap, or convert that asset.

## Changes in v2

- **Canonical state:** accounts with zero balance and zero nonce are never stored, so the state root no longer depends on whether an empty account "exists".
- **Timestamps:** strictly increasing; the caller may pass `max_timestamp` (e.g. wall clock + drift) so the core stays deterministic.
- **`Chain::validate_block` / `accept_block`:** re-execute a block built elsewhere and check version, chain ID, height, previous hash, timestamp, size, every transaction and the claimed state root. Failure leaves the chain untouched.
- **Block size bound:** `MAX_BLOCK_TXS` (prototype value).
- **Keys:** `secure_wipe` (OpenSSL `OPENSSL_cleanse`); the seed is wiped if key generation fails; the demo wipes its seed.
- **Housekeeping:** missing `<string_view>` include added, unused include removed, error messages name the failing tx index.
- **Tests:** from 1 scenario to 11 groups (expiry, insufficient balance, self-transfer, key/address mismatch, forged signature, nonce skip, overflows, atomicity with a differing second tx, timestamps, block size, canonical state, hash commitments, validate/accept with 11 tampered-block cases, SHA-256 known answer). Also run clean under `-fsanitize=address,undefined`.

Sanitizer run:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-san --parallel && ./build-san/taz_tests
```

## v3 notes

- Merged: the v2 core hardening above plus a GitHub Actions workflow (`.github/workflows/ci.yml`) that builds, runs the tests, and runs the tests again under ASan/UBSan.
- **Not included:** a fixed maximum supply. A supply cap (and its unit/decimals) is a token-policy decision that has not been made yet; add it deliberately, with a genesis rule and an invariant test.
- Empty blocks are allowed (a real chain needs them when there is nothing to include).

## v4: JSON export, wallet statement, offline explorer

- `taz::to_json(chain)` writes the whole chain (numbers as decimal strings, bytes as hex, correctly escaped).
- `taz::wallet_statement(chain, address)` is computed from the blocks only and is tested to equal the real state for every address.
- `taz_export_demo [out.json]` builds a throw-away chain with fresh random keys and writes it as JSON (keys are wiped, never saved).
- `explorer/index.html` is a single file with no server and no network access. Open it in a browser, load a JSON file, and it re-verifies in the browser: block links, block hashes, transaction IDs, address/public-key binding, Ed25519 signatures (needs a recent browser), the rules (nonce, balance, fee, expiry) and the state root after every block. A tampered file is reported with the exact reason.
- All file data is shown with `textContent` only (no `innerHTML`), so a hostile file cannot inject script.
- Regenerate the page with another chain: `python3 explorer/build.py my_chain.json`.
- Amounts are shown as integer `utzv`; decimals and total supply are still undecided policy.

## v5: account safety rules (opt-in, per account)

Every account may switch on its own protection. Nothing changes for an account that does not.

- **Daily limit** (`daily_limit`): at most this much can be sent per window of `LIMIT_WINDOW_BLOCKS` blocks. A held transfer counts when it is sent.
- **Hold large transfers** (`hold_threshold`, `hold_blocks`): a transfer of at least the threshold does not arrive at once. The amount leaves the sender immediately and waits inside the chain; it arrives at the start of block `sent_height + hold_blocks`. Until then the sender can take it back with a `CancelHeld` transaction (this is the "scam or mistake" escape hatch).
- **Loosening is slow, tightening is instant**: raising the limit, raising the threshold, shortening the hold or switching a rule off only takes effect after `SAFETY_DELAY_BLOCKS`. Tightening, or sending the current settings again (which also cancels a waiting loosening), is immediate. So a stolen key cannot lift the limits at once.
- New transaction kinds: `SetSafety`, `CancelHeld` (signed under a separate domain string `TAZVERA-TX-SIGNATURE-V2`; plain transfers are byte-for-byte unchanged).
- State root: states that use none of this keep the old v1 encoding; otherwise a fully length-prefixed `TAZVERA-STATE-V2` encoding is used.
- Supply is conserved: `circulating + held + burned == total` is checked after every block in the tests.
- The explorer re-runs all of these rules in the browser and was cross-checked against a 372-block random chain produced by the C++ tests.
- Wallet statement now replays the chain and understands held / released / cancelled entries.
- Tests: `taz_safety_tests` (shape rules, limit windows, hold and release, cancel, delayed loosening, atomic blocks, state roots, statement, and a 900-step pseudo-random run in which a second node must reach the same state root after every block). I also broke the code on purpose in four ways and confirmed the tests catch each one.


Parameters (`LIMIT_WINDOW_BLOCKS`, `MAX_HOLD_BLOCKS`, `SAFETY_DELAY_BLOCKS`) are prototype values counted in blocks; real values depend on the block time, which is still to be chosen.

## v6: guardian recovery ("परिवार से वापसी", opt-in, per account)

If the key is lost, trusted people ("guardians") can bring the account's funds to a NEW address (a new key). Nothing changes for an account that does not use it.

- `SetGuardians`: the owner names 1 to `MAX_GUARDIANS` (5) other accounts and a threshold ("2 of 3"). The list must be strictly ascending, contain no zero address and not the account itself.
- `RecoveryVote`: a guardian (signing with his own key) votes "move account X to new address Y". Voting again replaces his vote; the same guardian never counts twice. When `threshold` guardians name the same new address, the recovery is scheduled for block `now + RECOVERY_DELAY_BLOCKS` (300).
- `RecoveryCancel`: the account's own key can stop votes or a scheduled recovery while it waits.
- At the scheduled block every spendable utzv moves to the new address. The old account keeps its nonce (old signatures stay useless), its guardians are removed, and the new address starts with no safety rules or guardians. Funds still being held keep their normal course (they were already on their way to someone else).
- **Changing the guardians always waits** `SAFETY_DELAY_BLOCKS` (500) unless the account had none yet; sending the same list again cancels the change. Recovery (300) is deliberately shorter than that delay, so guardians that already agreed finish before a thief's swap of the list takes effect (tested).
- A recovery cannot be started while one is waiting, and the guardians cannot be changed while one is waiting.
- If the new address could not hold the funds (a state that a supply-conserving chain does not reach), nothing moves, the recovery ends and the guardians stay.
- Transaction kinds 3, 4, 5 use the same `TAZVERA-TX-SIGNATURE-V2` domain with the kind byte; export format becomes `v3` only when they are used; state root becomes `TAZVERA-STATE-V3` only when a guardian field is non-empty (v1/v2 chains keep their old roots).
- Tests: `taz_recovery_tests` (shape rules incl. unused-field checks, thresholds 1/2/3, vote changes, exact timing one block before and at the due block, owner cancel, delayed guardian change, thief scenario, overflow and empty-account edges, held funds, statements, state roots, and a 3500-step random run in which a follower node must match every state root; supply is checked after every block).

**Honest limits (read before relying on this):**
- Without a veto key, anyone who holds the account's key can cancel a recovery, so a thief with the key can contest the guardians. Use the veto key below.
- Any `threshold` guardians together can move the funds after the delay. Choose independent people.
- Recovery moves funds to a new address; it does not give the old address a new key (the address is derived from the public key).
- Prototype values only; no audit; no real funds.

## v6.1: local transaction pool and safe chain-file saving (from the Grok review package)

- `Mempool` (`include/taz/mempool.hpp`): a local waiting room for transactions. `submit` checks a transaction against the real state PLUS everything already waiting (so nonces must follow each other, daily limits and guardian rules see pooled transactions), refuses duplicates, a full pool and too many per sender; `select` builds a block's worth in order; `prune_stale` removes entries that can no longer apply (added by me: the original kept dead entries forever); `drop_expired`, `remove_included`.
- `save_chain_json` (`include/taz/persist.hpp`): writes the explorer JSON through a temporary file and a rename (changed by me: the original overwrote the file in place, so a crash could leave half a file).
- Not done: loading a chain back from JSON, networking. The `json_io.hpp` header in the Grok package had no implementation, so it is left out.
- Known cost: `submit` copies the state and replays the pool, so it is O(pool size) per call. Fine for a prototype, to be redone before any network use.
- Tests: `taz_mempool_tests` (nonce gaps, duplicates, bad signatures, tampered amounts, expiry, capacity, per-sender limit, safety and guardian rules inside the pool, stale pruning, saving).

## v7: veto key (optional, per account)

`SetGuardians` may name one extra address, the **veto key** (not the account itself, not a guardian). Idea taken from the ChatGPT design review.

- When an account has a veto key, **only the veto key can cancel a recovery**. The account's everyday key cannot, so a thief who stole it cannot block the guardians.
- The veto key cancels with `RecoveryCancel` whose `to` is the protected account (it signs and pays the fee with its OWN account, so the veto account needs a little balance). Leave `to` empty for the account's own cancel (only allowed when no veto key is set).
- Setting, changing or removing the veto key follows the same rules as the guardian list: first setup is immediate, any change waits `SAFETY_DELAY_BLOCKS`, resending the current setting cancels a waiting change. A change is refused while a recovery waits. The veto key is retired together with the guardians when a recovery completes.
- The veto address is part of the signed bytes and of the state root. **This changes the `TAZVERA-STATE-V3` and kind 3/5 signature layout compared with v6**, so chain files made by v6 do not verify; acceptable because there is no mainnet and no real data.
- Limit: whoever holds the veto key can stop every recovery, so keep it offline and separate from the everyday key; losing BOTH keys while the guardians are unavailable still loses the account.
- Tests: shape rules, only-the-veto-may-cancel (owner key, stranger, guardian, veto of another account all refused), retargeting after signing refused, delayed change, thief scenario, statements for both accounts, state root, and the random run mixes veto settings and veto cancels (29 accepted veto cancels in the checked run).


## Persistence (v0.8)

- `save_chain_json` / `load_chain_json`: explorer JSON, atomic save, full replay on load.
- Format: `tazvera-chain-export-v1|v2|v3` (feature level only; protocol_version must match).
- Private keys are never stored. Tampered files must fail verification.
- Local prototype only — not a networked consensus database.
