# F4 — Connection/sync cost limits (design only)

## Scope and current-main finding

Reviewed against main at `6c03457`.

**Current behavior:** the node has a block-based sync path, not the old whole-chain transfer path. It compares peer height and tip hash, skips transfer when the tip matches, and otherwise exchanges bounded batches (up to 200 blocks per batch). Receive work is bounded by a 60-second deadline and 4 MiB byte budget; each received block is parsed and validated against the local chain. These are useful per-sync work limits, but they are not connection/request rate limits. The `--peer` client currently dials only `127.0.0.1:<port>`.

**Still open (F4):** no connection/request rate limit or cooldown/backoff is implemented. Repeated reconnects or repeated sync attempts can still consume bounded work repeatedly. This document is a design proposal for the remaining resource-abuse controls, not a claim that all F4 protections exist. Recheck the implementation before applying this design to a later commit.

## Threat and invariants

A remote peer must not be able to multiply CPU, memory, or disk work by reconnecting, retrying sync, or advertising a long chain. Limits must be enforced before expensive JSON parsing, chain replay, or state-root recomputation. An authenticated key proves control of that key; it does not make the peer trusted.

## Options

| Option | Benefit | Cost / limitation |
|---|---|---|
| Per-source connection token bucket, with a global accept cap | Cheaply limits reconnect floods before identity proof | IP is not a stable identity; NAT can group honest peers and source addresses can vary |
| Per-authenticated-peer connection and sync-attempt budget | Stops one identity repeatedly requesting expensive work | Must authenticate first; Sybil peers can create many keys unless a global/IP cap also exists |
| Cooldown/backoff after failed or redundant sync | Avoids repeated parsing of the same unchanged peer tip | Can delay recovery after a peer's chain genuinely advances; state must expire safely |
| Advertised known tip: height + tip hash + genesis pin; compare before sync | Same tip means no full sync; same known prefix can request only missing blocks | Metadata is untrusted until checked; fork handling and incremental validation are more complex |
| Hard byte, block-count, parse-time, and total-work budgets | Bounds worst-case memory/CPU independent of peer identity | Requires careful limits so valid large blocks/chains are not rejected |
| Cache validated chain/tip results | Avoids repeating identical work | Cache invalidation, storage, and hash-key correctness become security-sensitive |

## Recommendation for the remaining F4 work

The existing tip comparison, bounded block batches, receive deadline/byte budget, and per-block validation should be retained. Add layered connection/request controls, not a single per-peer limit:

1. **Before authentication:** global concurrent-handshake cap, per-source-IP token bucket, bounded frame/stream sizes, and a single absolute handshake deadline. Keep the current pre-allocation length checks.
2. **After authenticated hello:** maintain a bounded per-identity connection/sync budget plus a global sync-work budget. Charge an attempt before parsing; impose a cooldown for failed or unchanged-tip requests.
3. **Tip-first protocol:** exchange chain ID/genesis pin, height, and tip hash in a bounded authenticated status message. If the known tip matches, do no chain replay. If it differs, request bounded missing blocks (or a bounded checkpoint/snapshot format only after it has its own validation design). Validate each received block once against the local tip; never trust a peer's claimed height/hash as proof.
4. **Bound work:** cap outstanding syncs, response bytes, blocks per batch, and total validation work/time. Reject over-budget requests before allocating or replaying a chain. Make limits configurable only within safe hard ceilings.
5. **Observability:** expose counters for rejected connections, redundant syncs, bytes, blocks validated, and budget exhaustion; avoid logging private keys or full sensitive payloads.

### Tests required before the remaining F4 work is accepted

- Repeated connections from one source hit the rate limit; another source still connects.
- Many source addresses cannot exceed the global handshake/concurrency cap.
- One authenticated identity cannot exceed its sync-attempt budget; budget resets after the documented window.
- Invalid/unauthed identity cannot consume the post-auth sync budget or start chain verification.
- Same genesis + same height + same tip triggers zero full-chain replay and zero block requests.
- Same tip with a peer lying about height, or same height with a different tip, never skips validation.
- Incremental sync validates every block once, rejects bad predecessor/hash/state root, and does not mutate state on rejection.
- Oversized frame, oversized logical message, too many blocks, slow-trickle peer, timeout, and exhausted-work budget fail boundedly without unbounded allocation.
- Reconnect/retry storms keep CPU, memory, file writes, and active sockets within configured ceilings.
- Two peers advertising different forks are handled deterministically; invalid chain/genesis is rejected before expensive replay.
- Regression: valid maximum-sized block and legitimate catch-up sync still succeed.

**Not a consensus change:** these are connection/sync resource controls only. The sync/fork behavior must preserve the existing chain-validation rules.
