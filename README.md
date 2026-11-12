# TAZVERA Chain — Local Prototype

**Not mainnet. Not the Shardeum ERC-20 TZV. Demo keys only.**

## Phase 1
Genesis, signed transfers, fee burn, nonce/signature checks.

## Phase 2 — Package 1
- Canonical **JSON** export/import for transactions and blocks
- **Mempool**: reject duplicates, bad signatures, bad nonces
- **Multi-transaction blocks** + `seal_from_mempool`

### Not in Package 1
TCP multi-node, disk persistence, 21 validators, BFT.

## Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/taz_tests
./build/taz_demo
```
Needs C++20, CMake, OpenSSL.

## Fee schedule v1
Transfer fee: 1000 utzv (0.001 TZV), 100% burned.

Independent reviewers should build from source and run tests. Chat logs are not an audit.
