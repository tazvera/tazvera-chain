# Run a TAZVERA node on your machine

This guide builds the node from repository source using CMake; no downloadable release has been published yet.
The current CLI provides `--produce-every`; with `--serve`, it attempts to produce a block every N seconds. The `--tls` flag/transport is not implemented yet.
Note: `--every <seconds>` is only the `--peer` reconnect interval, not the block-production interval.
This is a local development prototype, not a public testnet or payment service. Balances/transactions here are test data, not economic value. Do not share private keys/seeds. In this guide, both nodes run **on the same computer**.

## 1. Build from source

No release binary is currently available to download. Clone the repository source and build the binary with CMake. For a two-machine test, compatible binaries must be built from the same commit on both machines.

Linux:
```sh
git clone https://github.com/tazvera/tazvera-chain.git
cd tazvera-chain
git checkout main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows 10/11, open an MSYS2 UCRT64 terminal and install dependencies according to the project's Windows build instructions, then run CMake configure/build/test. The compiler must support C++26. Do not continue if build/tests fail.

## 2. Create test keys and genesis

Create two test keys from the project root:
```sh
./build/taznode --new-key node-a.key
./build/taznode --new-key node-b.key
```
On Windows, the executable is `build/taznode.exe`. Note both printed public addresses. Do not share the private key files.

Create a new chain; replace the addresses with the actual test addresses:
```sh
./build/taznode --chain local-chain.json --init-genesis ADDRESS_A:100000000 --init-genesis ADDRESS_B:100000000
```
Safely record the genesis commitment/pin exactly as printed. Different addresses, balances, or options produce a different pin. An existing chain file is not overwritten; use a new filename if needed.

## 3. Verify the pin

```sh
./build/taznode --chain local-chain.json --expect-genesis <pin>
```
Replace `<pin>` with the complete genesis commitment. The chain should load with the correct pin; do not continue if the pin is wrong.

## 4. Run a block producer

`--produce-every` is available. Run the producer with `--serve` and a trusted genesis pin. On a producer-enabled chain, `--key-file` must contain the private key of a producer configured in that chain.

Example — attempt to produce a block every 5 seconds:
```sh
taznode --chain a.json --expect-genesis <pin> --key-file a.key --serve 7001 --produce-every 5
```
Replace `<pin>` with the actual trusted genesis pin. `a.key` must be the key of the producer configured in the chain. This command keeps running; press Ctrl+C to stop it.

**Limitations / required behavior:**
- Blocks are scheduled every N seconds, but timing may drift. The node is single-threaded; lengthy peer work can delay the next tick.
- These blocks will be empty. `taznode` has no mempool, so pending transactions are not automatically included in these blocks.
- On a chain with configured producers, the configured producer's key is required to produce blocks. A tick may be refused/skipped with another producer's key.
- If the tip timestamp is 120 seconds or more ahead of the local clock, the node prints a warning and skips that tick; it waits for the clock to catch up.
- If the block cannot be saved to the chain file, the node exits with code 3.
- Supplying `--produce-every` without `--serve` is a usage error (exit code 2).
- Running two producers on the same chain can create a fork race. The intended setup for this option is one producer and followers; this is not consensus/finality.

## 5. Run two nodes on the same machine

Open two terminals. Make a separate copy of the chain file for the peer node:

```sh
cp local-chain.json peer-chain.json
```

In Windows Command Prompt, run `copy local-chain.json peer-chain.json`.

**Terminal 1 — server/producer:** In the producer command above, use `local-chain.json` instead of `a.json` and keep port `7001`.

**Terminal 2 — peer:**
```sh
./build/taznode --chain peer-chain.json --expect-genesis <pin> --peer 7001
```

On Windows, use `build/taznode.exe` as the executable. Keep both server and peer terminals open; check the logs for connection/sync. Give the peer a separate copy of the producer's chain file; do not let two processes write to the same file.

## 6. Important note about separate machines

`--listen-on <IPv4-address>` is available; use it with `--serve <port>` or `--listen <port>`. The default listener address is `127.0.0.1`. `--peer <port>` currently connects only to `127.0.0.1:<port>`; the remote-address option `--peer <address>:<port>` is not available yet. Therefore, run two nodes on the same machine.

## 7. Common mistakes

- **Genesis pin mismatch:** Use the same complete pin on both nodes; genesis inputs must also match.
- **Port busy / bind failed:** Stop the old node or choose an unused port; use the same port in both commands.
- **Unknown flag:** Rebuild the binary from the correct commit and check `--help`.
- **Height does not increase:** Check the producer logs; verify the configured producer key, clock warnings, and chain-file write errors.
- **Chain load/parse error:** Stop the node and preserve the logs; do not edit the chain file manually. Create genesis with a new filename only for disposable test data.

When the test is finished, stop the node terminals with **Ctrl+C**. Do not modify the chain file manually or share private keys.
