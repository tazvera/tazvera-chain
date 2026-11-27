# TAZVERA Chain Core v6.2 — height-overflow hardening patch

This patch is prepared against the `main` branch state inspected on 15 Nov 2026.

## Files to replace
- `src/chain.cpp`
- `src/mempool.cpp`
- `tests/test_mempool.cpp`

## What it changes
- Stops block creation when the chain height is already `UINT64_MAX`.
- Stops incoming-block validation from evaluating `tip.height + 1` when that addition would overflow.
- Makes the mempool reject submissions at `UINT64_MAX` with `chain height exhausted`.
- Makes mempool selection return no transactions when no next block height can exist.
- Makes stale-pruning leave the pool unchanged at that boundary.
- Adds a regression test for the mempool boundary.

## Important limits
- This patch does not change Guardian Recovery policy. That needs a carefully specified threat model and dedicated tests; a quick change could lock legitimate owners out or create a recovery bypass.
- This patch does not fix concurrent chain-file saves; that needs platform-aware atomic-file handling and dedicated tests.
- This ZIP has not been built or run in this environment. Run the repository CI/build before merging.

## Build and test
From the repository root:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Use a new branch and a pull request. Do not upload these files directly to `main`.
