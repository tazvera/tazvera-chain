# F5 — Open decisions and explicit scope

## Fixed behavior for this PR

- When two peers have the same genesis and height but disagree on history, the current implementation can only test whether the peer's advertised tip attaches directly to the local chain after removing the local tip.
- If that attachment fails, the node must reject the sync, emit a clear refusal message, and return a non-zero result. It must not report the peer as successfully served or the sync round as successful.
- This PR does not attempt a common-ancestor search or a multi-block reorganization.

## Deferred decisions

1. **Common-ancestor reorganization:** a separate PR must define and test how to locate the highest common ancestor, validate every replacement block in order, and commit the replacement atomically. Until then, deeper forks are refused.
2. **Fork choice and finality:** the existing height/tip-hash selection rule is only a deterministic tie-break, not consensus or finality. It must not be described as consensus.
3. **Peer retry policy:** whether a node should automatically retry a refused deep fork, and with what backoff/limit, remains a separate networking decision. This PR only guarantees that the failed sync is not reported as success.
4. **Unequal-height divergent histories:** this PR's regression tests cover equal-height deep forks only. A separate test/review must define behavior when a longer peer chain diverges before the local tip; no common-ancestor reorg is introduced here.

## Validation boundary

- Regression cases: divergence at block 1, with both chains at heights 2, 3, and 10.
- Expected result: non-zero exit from the connecting node, explicit "unrepaired deep fork" refusal, and neither chain file modified by the failed exchange.
- No changes to networking protocol, consensus, persistence format, or common-ancestor reorganization.
