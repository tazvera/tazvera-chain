# TAZVERA Chain — Core Repair v2

**Local prototype only. Not a testnet or mainnet. Not the existing Shardeum Mainnet TZV asset. Do not use real funds or production wallet keys.**

This package is a small C++20 deterministic account-state prototype. It is deliberately limited to genesis, signed transfers, fee accounting, block construction and local tests. It does not implement P2P networking, consensus, validators, staking, RPC, persistence, wallet software, JSON protocol or smart contracts.

## Included

- Ed25519 signing and verification using OpenSSL EVP.
- SHA-256 hashing using OpenSSL EVP.
- Signatures bind protocol version, chain ID, sender, recipient, amount, fee, nonce, expiry and public key.
- Sender address/public-key binding, fixed prototype fee and integer overflow checks.
- Deterministic state ordering and state root.
- Block hash commits to metadata, transaction IDs and state root.
- Atomic block application: a rejected block cannot partially change live state.
- Genesis supply cap of 100 crore TZV at 6 decimal places, expressed in integer `utzv`.
- CMake build, CTest suite and GitHub Actions CI.

## Build locally

Requirements: C++20 compiler, CMake 3.20+, OpenSSL 3.x development package.

```bash
sudo apt update
sudo apt install build-essential cmake libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/taz_demo
```

## Prototype fee

The local transfer fee is `1000 utzv` (0.001 TZV) and is burned in this model. This value is for local tests only; it is not an approved mainnet fee. Validator reward economics remain unresolved because 100% fee burn does not pay validators.

## Security limitations

A successful build/test is not a security audit. The prototype has no consensus or network threat model, no peer-to-peer implementation, no persistence/recovery, no fuzzing campaign, no independent review and no audited address standard. Do not deploy with real funds. Use established cryptographic libraries; do not invent cryptographic primitives.

Keep this independent-chain prototype separate from the existing Shardeum Mainnet TZV. There is no bridge, migration or automatic conversion.
