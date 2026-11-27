# TAZVERA Chain Local Prototype: Status and Limitations

This repository contains a local prototype built from source with CMake; no downloadable release has been published yet.
`--produce-every <seconds>` is available: with `--serve`, the producer attempts to create a block every N seconds. The `--tls` flag/transport has not been implemented yet.
`--every <seconds>` is only the `--peer` reconnect interval, not the block-production interval.
The Explorer testnet page, `explorer/testnet.html`, shows sample/demo data and is not connected to a running node.

## What is this?

This is the repository's local prototype, not a public testnet or mainnet. It is separate from the existing Shardeum TZV token. Balances and transactions here are local test data only; they have no declared economic value, market price, or redemption value, and this is not a real-world payment service.

## Requirements for running a node

- A C++26-capable compiler, CMake 3.20+, and OpenSSL 3.x development files are required.
- No release binary is available yet; check out the source and build with CMake. Full instructions: `docs/RUN-A-NODE.md`.
- On Windows, use an MSYS2 UCRT64 terminal and its matching compiler/OpenSSL packages.
- A trusted genesis commitment must be provided whenever opening an existing chain.
- The node listener defaults to `127.0.0.1`. `--listen-on <IPv4-address>` can be supplied with `--serve <port>` or `--listen <port>`; it changes the listener's bind address.
- `--peer <port>` currently connects only to `127.0.0.1:<port>`. The remote-address option `--peer <address>:<port>` is not available yet; therefore, peer connections between two separate computers are not supported yet.
- `--produce-every <seconds>` is available and can only be supplied with `--serve`. Examples and operating instructions: `docs/RUN-A-NODE.md`.
- The `--tls` CLI flag is not available, and TLS transport has not been implemented yet.

## Producer flag behavior and limitations

Example:
```sh
taznode --chain a.json --expect-genesis <pin> --key-file a.key --serve 7001 --produce-every 5
```

- Blocks are scheduled every N seconds, but timing may drift: the node is single-threaded, and a long peer exchange can delay a tick.
- Blocks will be empty; `taznode` has no mempool, so transactions are not automatically included in these blocks.
- A producer-enabled chain requires the configured producer's key. A tick may be refused/skipped when using another producer's key.
- If the tip timestamp is 120 seconds or more ahead of the local clock, the node issues a warning and skips that tick; it waits until the clock catches up.
- If a block cannot be saved, the node exits with code 3.
- Supplying `--produce-every` without `--serve` is a usage error (exit code 2).
- Two producers on the same chain can create a fork race. This flag does not provide consensus/finality; the intended setup is one producer and followers.

## What works now

- Create a local chain file and load it with a trusted genesis pin.
- Ed25519 transaction signing/verification and prototype transfer fee/nonce/balance checks.
- Create local blocks, save/load them in the chain file, and check chain state/hash.
- Sync blocks with a localhost peer on the same machine.
- Authenticated hello/peer identity handshake and configured peer identity allowlist.
- Optional read-only HTTP API; disabled by default and listens only on loopback.
- The CLI includes `--listen-on`, `--serve`, `--listen`, `--peer`, `--api`, `--register-asset`, `--transfer-asset`, and `--produce-every`.

## What is not available yet / what not to rely on

- This is not a public testnet or production network; there is no public endpoint or public peer network.
- No remote-address peer dialing; `--peer` is currently localhost-only.
- No production-grade consensus/finality, fault-tolerant producer availability, TLS transport encryption, or connection/request rate limiting.
- The HTTP API has no transaction submit/write endpoint.
- The prototype transfer fee is 1000 utzv and is burned in the local model; this is not a final or approved fee.
- Passing local builds/tests is not evidence of a security audit or production readiness.

## If something goes wrong

1. Stop the node and note the exact error message.
2. After changing the build, stop the old process and rebuild the binary: `cmake --build build --parallel`.
3. If the chain does not load, provide the correct trusted genesis pin. Do not derive the pin by guessing from the chain file.
4. If a peer does not connect, check that both commands use the same port, the same genesis pin, and the correct chain file. `--peer` currently works only on localhost; do not try connecting from a separate computer.
5. If the port is busy, stop the server or choose an unused local port and use the same port on both sides.
6. If a transfer fails, check the sender's balance for the amount plus the 1000 utzv fee and the correct nonce.
7. If the cause is not clear from logs/error, file an issue with reproduction steps and error output. Never attach a secret key file or private seed.

**Security note:** This software is for testing. Do not treat local transfer functionality as evidence of real-world payment, economic value, or production readiness.
