# TAZVERA Chain — Core v5

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

**Not built yet:** guardian recovery (3-of-5 trusted people restoring an account after a lost key). Until it exists, a lost or stolen key cannot be recovered.

Parameters (`LIMIT_WINDOW_BLOCKS`, `MAX_HOLD_BLOCKS`, `SAFETY_DELAY_BLOCKS`) are prototype values counted in blocks; real values depend on the block time, which is still to be chosen.
