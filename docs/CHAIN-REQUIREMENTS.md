# CHAIN REQUIREMENTS — master checklist for a real chain

# Master checklist: what a real chain needs—and what it does not

Basis: review of the current main-branch source and available project docs. An existing code path is not proof of production readiness or a security certification.

Status: ✅ implemented · ⚠️ partial/limited · ❌ not implemented

## A. Must-have

| # | Requirement | Code-checked status |
|---:|---|---|
| 1 | Nodes on two machines connect. | ❌ `--peer` dials only `127.0.0.1:<port>`. `--listen-on` allows external inbound connections, but outbound peer addressing is not available. |
| 2 | Blocks are produced automatically with `--produce-every`. | ✅ Empty blocks are produced with `--serve`; producer chains require the correct key. |
| 3 | A second node synchronizes. | ✅ Peer sync exists; the recorded test was local, not cross-machine. |
| 4 | Disk persistence and the same chain after restart. | ✅ JSON save/load, replay, and genesis pin checks exist. |
| 5 | Signed transfers, fees, and nonces. | ✅ Transaction validation checks these. |
| 6 | Signed node identity hello. | ✅ Identity proof and connection-bound nonce checks exist; enforcement is enabled with `--require-identity`. |
| 7 | Peer allowlist. | ✅ `--allow-peer` / `--allow-peer-file`, used with `--require-identity`. |
| 8 | Replay protection and a separate chain_id for every network. | ⚠️ Chain ID and genesis commitment are bound into messages/commitment; however, CHAIN_ID is currently a fixed compile-time constant, not configurable per network. |
| 9 | Explicitly reject deep forks. | ✅ There is a refusal/non-zero failure path for an unrepairable peer tip. |
| 10 | Resolve forks through reorganization. | ❌ No common-ancestor search or multi-block atomic reorganization; deep forks are rejected. |
| 11 | Chain continues if a producer stops (F7). | ❌ No producer failover/consensus takeover. |
| 12 | A slow/hostile peer cannot stall the network (F4). | ❌ F4 connection-cost/rate-control work remains; timeouts do not complete it. |
| 13 | Encrypt peer traffic with TLS. | ❌ TLS context and TlsTransport adapter exist, but the node peer path does not use them; traffic is plaintext. |
| 14 | Guardian recovery. | ✅ Guardian recovery logic exists. |
| 15 | RWA asset record. | ⚠️ Asset registry/registration/transfer exists; this is not legal title or a complete RWA platform. |
| 16 | Chunk large messages. | ✅ Bounded chunks and reassembly checks exist. |
| 17 | One writer per chain file. | ✅ Permanent sidecar writer lock exists. |
| 18 | A far-behind node can sync. | ⚠️ Missing-block sync exists; performance/scale for large lag is unproven. |
| 19 | Wallet transaction submission via POST /tx. | ❌ The API is read-only/GET-only; there is no POST /tx route. |
| 20 | Explorer displays data from the live API. | ❌ Explorer verifies a loaded JSON file; it is not connected to the live API. |

## B. Must never happen

| # | Rule | Code-checked status |
|---:|---|---|
| 1 | No "real money" or rupee claim. | ✅ Project docs describe a local prototype/token model; this is a product-claims rule, not a consensus invariant. |
| 2 | No continuation with disk/state mismatch after a save failure. | ✅ Producer exits on save failure; persistence reports failure. |
| 3 | Never accept an unsigned transaction. | ✅ Signature and public-key/address binding are validated. |
| 4 | No public API exposed without TLS. | ⚠️ API is loopback-only; peer TCP is plaintext. Do not expose the API publicly. |
| 5 | No unbounded connections/resource capture (F4). | ⚠️ API has a connection cap; peer-network F4 remains unfinished. |
| 6 | Never load old chain data without protocol/genesis checks. | ✅ Loader checks protocol, chain ID, and genesis commitment/pin; unsupported versions are rejected. |
| 7 | Never overwrite an existing file with fresh genesis. | ✅ Exclusive creation refuses an existing file under the writer lock. |
| 8 | Do not hide operational limits from operators; document them. | ⚠️ Known limits are recorded here; future limits must also be added to the docs. |

## C. Remaining work, in order

### To start a testnet

1. `--peer <address>:<port>` — currently, `--peer <port>` is loopback-only.<br><small>Add remote peer address support.</small>
2. Test connection and sync across two separate machines.<br><small>Test cross-machine connection and sync.</small>
3. F7 — producer failover/continuity.<br><small>Producer failover/continuity.</small>
4. F4 — connection-cost/slow-peer protection.<br><small>Connection-cost and slow-peer protection.</small>
5. Integrate TLS into the node peer path and test the handshake.<br><small>Integrate TLS into the peer path and test handshake.</small>

### To make it useful

6. Signed transaction submission API: POST /tx.<br><small>Signed transaction submission API.</small>
7. Connect Explorer to the live/read-only API.<br><small>Connect Explorer to the live read-only API.</small>
8. Configurable chain_id for each network.<br><small>Configurable chain ID per network.</small>
9. F9 — resolve funds stuck in recovery.<br><small>Resolve funds stuck in recovery.</small>
10. F12 — address the cost/behavior of writing the full chain on every block.<br><small>Address full-chain write cost/behavior per block.</small>

### Later

Consensus, Digital ID, and marketplace. These are not considered implemented in this checklist.

## Review boundary

"Implemented" means a code path exists; it does not prove cross-machine validation, production security, consensus/finality, economic safety, or legal validity.
