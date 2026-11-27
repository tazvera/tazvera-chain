# DESIGN: POST /tx — signed transaction submission
<small>Design only. No implementation in this change.</small>

## Purpose and scope

This document proposes the next API work. The current HTTP API is read-only and GET-only; `POST /tx` is not an available endpoint yet. This design is only for accepting signed transactions. It does not authorize enabling consensus, block production, TLS, or remote API access.

## Endpoint and request format

- Method/path: `POST /tx`
- Content-Type: `application/json`
- Body shape: `{"tx":"<hex>" }`
- `tx` contains the lowercase hexadecimal representation of the bytes of one complete signed transaction's canonical wire serialization, including its signature.
- Serialization/version, byte order, field lengths, and exact decode rules must be fixed in a protocol document before implementation. Do not accept JSON fields as separate transaction fields; the signature must be verified over those exact serialized bytes.
- Reject requests with malformed, non-canonical, or trailing bytes after hex decoding. Never send a private key/seed in the request.

Successful first admission: `202 Accepted`, with a transaction ID and status such as `{"txid":"<64-hex>","status":"queued"}`. This means queued, **not confirmed**. Invalid request: appropriate 4xx with a short stable error code; do not expose secrets or internal stack traces.

## Server validation order

Before placing a transaction in the mempool:

1. Check the request method, Content-Type, JSON shape, hex syntax, and decoded byte length.
2. Decode the canonical transaction; reject unknown version/kind and non-canonical encoding.
3. Check the protocol version and `chain_id`; do not accept a transaction from another chain.
4. Check that the sender address matches the public key; verify the signature over the exact signed bytes.
5. Check the transaction shape, destination, amount overflow, and fixed fee schedule. The fee must match the current rules of this chain; the client must not be able to choose its own fee.
6. Check the sender's current nonce and available balance. The balance must cover both amount + fee. The nonce must be checked not only against current chain state but also against the ordering of that sender's transactions already queued in the mempool.
7. If expiry is enabled, check it. Only then place the transaction in the bounded mempool.

These checks are preliminary at admission time; all consensus-validity checks must be applied again when producing/accepting a block. Admission validation is not block confirmation.

## Replay and duplicate handling

- Derive the transaction ID from SHA-256 of the signed canonical bytes (including the signature).
- If the same txid is submitted again while in the mempool, do not queue it a second time. Return the existing txid and `already_known` status; do not treat the duplicate as a separate transaction that consumes another fee/nonce.
- A transaction already included in a block will have advanced the nonce in chain state; on resubmission, return `bad_nonce`/already-included and do not apply it.
- For one sender and one nonce, only one of multiple different signed transactions may have an active queue entry. Reject/replace conflicts deterministically; never silently keep both.
- When reorg/fork support is added, the included/queued index must be rebuilt according to the canonical chain. For this prototype, mempool behavior during forks/reorgs must be specified before implementation.

## Resource limits (proposed defaults)

These are proposed limits for the new POST endpoint, not claims about the existing GET API:

- Maximum HTTP request body: **64 KiB** (including JSON); larger bodies return `413 Payload Too Large` and the connection is closed.
- Maximum decoded signed transaction: **32 KiB**; reject larger transactions.
- Rate limit: **10 requests/second per loopback client**, burst **20**; exceeding it returns `429 Too Many Requests`. Count invalid requests too.
- Concurrent connections: keep the existing API limit of **8**; do not increase the total limit by creating a separate pool for POST.
- Request timeout: keep the existing **10 seconds**; close the connection when a slow body upload times out.
- Mempool: maximum **10,000 transactions** and **16 MiB** of serialized bytes; when full, return an explicit `503`/busy response and do not silently drop transactions.
- Pin these limits in tests before code/API implementation. Do not treat loopback-only deployment rate limits as public-network protection.

## Mempool versus producing a block directly

**Having the POST handler produce a block directly is not recommended.** It would make every request trigger block production, make outcomes depend on competing request order, complicate batching/nonce ordering, and let an API request mutate the chain directly.

Recommended flow: validate → bounded mempool → existing block producer/explicit block workflow selects transactions → block validation/commit → node synchronizes. Benefits: batching, deduplication, sender nonce ordering, and separation of API from block production. Costs: queued transactions are not guaranteed confirmation, mempool eviction/restart behavior is needed, and inclusion may be delayed.

The current node has no autonomous block producer/mempool submission API. Before implementing this design, producer integration, queue persistence policy, expiry/eviction, and shutdown behavior must be decided separately. Do not make the POST endpoint a shortcut that creates blocks itself.

## CORS and Host security

Keep the current API's CORS and Host validation rules unchanged:

- Host must be an accepted form of `localhost:<port>` or `127.0.0.1:<port>` on the configured port; reject duplicate/invalid Host.
- Origin must be a valid exact origin and present in the configured allowlist; do not accept wildcard `*`. Do not change the existing behavior for non-browser clients with no Origin.
- Do not add permissive CORS, wildcard origins, or Host-check bypasses just because POST is being added. CORS is not authentication.

## Network exposure / TLS

**This endpoint must remain loopback-only until TLS is implemented.** Keep the bind address limited to `127.0.0.1`; do not enable POST on `0.0.0.0`, a LAN, or a public interface. The existing API Host/CORS checks are not a substitute for TLS. Even after TLS is added, remote exposure requires a separate security review and explicit configuration.

## Tests required before implementation is accepted

1. Valid signed transaction is accepted and queued; response contains stable txid/status.
2. Invalid JSON, missing/extra fields, malformed hex, odd-length hex, non-canonical serialization, and trailing bytes.
3. Body exactly at the limit is accepted; one byte over returns 413; decoded transaction limit is enforced.
4. Unsupported protocol version, wrong chain_id, unknown kind, invalid shape, and wrong fee are rejected.
5. Valid signature is accepted; corrupted signature and wrong sender/public-key pair are rejected.
6. Exact duplicate while queued returns already-known and queue size does not grow.
7. Already-included transaction is not queued/applied again; nonce conflict from the same sender is handled deterministically.
8. Correct nonce is accepted; stale/skipped nonce is rejected; balance checks include amount + fee; fee burn occurs only when a block commits.
9. Queue full by count/bytes returns an explicit busy response; eviction/expiry and restart behavior match the documented policy.
10. Rate-limit burst/refill, invalid-request counting, and 429 response.
11. Connection cap, timeout, oversized slow upload, and connection cleanup.
12. Host spoofing, duplicate Host, disallowed Origin, wildcard Origin, and allowed exact Origin behavior remain unchanged.
13. Bind test proves the POST listener accepts loopback only; attempts to bind non-loopback are rejected.
14. An accepted/queued transaction does not change balance, nonce, block height, or state root until a valid block is committed.
15. The node synchronizes the committed block and independently validates the transaction; the API response is never described as confirmation.
16. Regression: existing GET endpoints and current GET-only behavior remain unchanged until this endpoint is intentionally implemented.

## Open decisions to resolve before coding

- Publish the exact canonical transaction wire format and a protocol/version identifier.
- Decide whether the mempool survives restart and how it reconciles with accepted blocks/forks.
- Pin stable error codes and duplicate/included response semantics.
- Confirm the 64 KiB / 32 KiB / 10 req/s / burst 20 / 10,000 tx / 16 MiB defaults against expected workload.
- Specify how the block producer selects transactions and handles sender nonce order.
