# How to verify a chain

This guide compares a received chain file with the recorded TAZVERA local prototype. It is not proof of legal ownership or a public network.

## 1. Genesis pin: obtain it independently

**Do not take the pin from the chain file.** Obtain it independently from a trusted source, such as a signed release/official published record or a record confirmed earlier through a separate channel. A pin found only in the same file is not verification; anyone can create a chain and its pin together.

Pin for this recorded local run:

```text
5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63
```

1. Copy the complete pin from the external record without altering it.
2. Open a copy of the received file with the trusted pin:
   ```sh
   taznode --chain received-chain.json --expect-genesis <trusted-pin>
   ```
3. Replace the placeholder with the real pin. Reject the file on mismatch/load error. A successful load only confirms the genesis commitment; continue checking.

## 2. State root: state after each block

`state_root` commits to the ledger/state after applying a block's transactions, such as balances and nonces. Apply blocks in order from genesis, recompute state at each height, and compare the resulting root with that block's `state_root`. Stop on any mismatch.

A root returned by the API is that node's claim, not an independent recomputation. A documented standalone verifier has not been confirmed for this prototype; do not treat API output as recomputation.

## 3. Block hash and chain links

A block hash is a digest of protocol-defined serialized block content, not an arbitrary hash of raw JSON. The implementation defines which fields and encoding are included.

- Inspect each block hash and previous-block hash/reference; each reference must match the immediately preceding block's hash.
- Changing a block's content should change its hash; the next block still refers to the old hash, breaking the link and subsequent chain-link checks.
- Reading a stored/API hash is not independent verification. Do not hash raw JSON with generic SHA-256 unless using the implementation's canonical serialization and algorithm.

## 4. Count transfers in a block

1. Fetch `GET /block/131` and count transaction entries. The recorded example has **one transfer** in block 131.
2. Inspect available sender, recipient, amount, fee, and nonce fields. Sender is k1, recipient is k2, and amount is 1000.
3. Compare the transaction entry with post-block balances; its presence alone does not prove correct application.

## 5. Read-only API checks

The API is disabled by default and listens on loopback. Replace `<api-base>` with the node's actual API base URL, such as `http://127.0.0.1:<port>`; use the configured port, do not guess.

```sh
curl <api-base>/status
curl <api-base>/block/131
curl <api-base>/balance/c68b2d0cb47a57e30834fc235b8422bfdc09d728
curl <api-base>/balance/f46c08954f95a092e225d33d9b3ee379c05c14b7
```

- `GET /status`: inspect available identity, protocol, and height fields. In this example, chain_id is `tazvera-local-prototype-v1`, protocol is `3`, and producer height is at least 131. Use the actual response field names.
- `GET /block/131`: check the returned height, hash, previous hash, state root, and transaction list fields; one transaction is expected.
- `GET /balance/<address>`: inspect both accounts' balance/nonce. After the recorded run, k1 was `998000`, nonce `1`; k2 was `1000`, nonce `0`.

Record any mismatch and do not trust that file/node. Multiple API responses from one node are not independent sources.

## 6. Short recorded example

- Genesis pin: `5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63`
- Block 131: one transfer of 1000 from k1 to k2
- Afterward, k1 balance `998000`, nonce `1`; k2 balance `1000`, nonce `0`
- Total deducted from k1: `2000` — `1000` transfer + `1000` fee

These are recorded local-run figures for 27 November 2026, not a guarantee for future runs.

## 7. What these checks do not prove

- **Legal ownership:** An address/balance does not identify the legal owner of an asset; legal documents and applicable law must be reviewed.
- **Consensus/correct operation:** These checks do not prove that the chain operated correctly, that all nodes agreed, or that no fork occurred. The prototype has no production consensus/finality; hashes and state roots do not create consensus.
- **Complete security:** A matching pin, API response, or hash links do not fully guarantee software, operator, key custody, network security, or economic claims.

**Safety:** This is a local prototype; do not run it on a public network and never share a private key/seed.
