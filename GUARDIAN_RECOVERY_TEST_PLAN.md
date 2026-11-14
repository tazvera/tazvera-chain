# TAZVERA Guardian Recovery — Test Plan v1

**Status:** Test specification only. No test is claimed to have run.

## A. Registration tests

1. Valid owner + veto-key proof + three distinct guardian keys succeeds.
2. Missing veto-key proof fails.
3. Duplicate guardian keys fail.
4. Guardian equal to active spend key fails.
5. Guardian equal to veto key fails.
6. Zero/invalid public keys fail.
7. Unauthorized guardian replacement fails.
8. Guardian replacement increments guardian epoch.
9. Old approvals fail after guardian epoch changes.
10. Failed registration/replacement leaves state root unchanged.

## B. Approval tests

1. One valid guardian approval is insufficient.
2. Two distinct valid guardian approvals succeed.
3. Same guardian signature supplied twice counts once only.
4. Unregistered guardian signature fails.
5. Invalid signature fails.
6. Signature over altered new key fails.
7. Signature over altered account ID fails.
8. Signature over altered chain ID/domain fails.
9. Stale guardian epoch fails.
10. Stale recovery nonce fails.
11. Expired request fails.
12. Duplicate request ID fails.
13. New key equal to current key fails.
14. New key equal to guardian or veto key fails.
15. Request height + delay overflow fails safely.
16. Third guardian approval does not change the result or create duplicate state.

## C. Delay and cancellation tests

1. Finalization before ready height fails.
2. Finalization exactly at ready height succeeds.
3. Active spend key can cancel during delay.
4. Independent veto key can cancel during delay.
5. Invalid veto signature fails.
6. Veto key cannot cancel another account's request.
7. Cancel with wrong request ID fails.
8. Cancel after finalization fails.
9. Cancel after cancellation is idempotently rejected or reported as already cancelled.
10. Expired pending request cannot finalize.
11. Cancellation clears pending state and leaves active key/balance unchanged.
12. Cancelled request cannot be replayed.
13. A stolen active spend key cannot cancel using the veto path unless it also has the independent veto key.

## D. Finalization tests

1. Anyone may submit finalize after the delay for the exact valid request.
2. Wrong request ID fails.
3. Finalization before delay fails.
4. Finalization after cancellation fails.
5. Finalization after expiry fails.
6. Finalization changes active key atomically.
7. Old key can no longer transfer after finalization.
8. Old key can no longer change safety settings.
9. Old key can no longer change guardians or start another recovery.
10. New key can transfer and manage the account.
11. Account ID and balance remain unchanged.
12. Authorization epoch and recovery nonce increment exactly once.
13. Repeated finalization fails.
14. Any failure leaves the state root unchanged.

## E. Replay, ordering, and determinism

1. Same approval cannot be reused for another request.
2. Same request cannot be replayed on another chain ID.
3. Same request cannot be replayed after guardian rotation.
4. Same request cannot be replayed after a later recovery.
5. Different transaction orderings that are valid under the protocol produce deterministic results.
6. Two independent nodes replay the same block and produce identical state roots.
7. Rejected block containing a recovery transition leaves no partial state changes.
8. State serialization is independent of map insertion order.
9. Every pending and finalized recovery field affects the state root.
10. Fuzz canonical decoding, signature bytes, lengths, counters, heights, and malformed transaction kinds.

## F. Property / invariant tests

- At most one pending recovery exists per account.
- At least two distinct current guardians approved every pending recovery.
- No guardian alone can initiate or finalize a recovery.
- A finalized recovery always waited until `ready_height`.
- A cancelled/expired request never finalizes.
- Account balance is unchanged by recovery itself.
- Old authorization epoch never becomes valid again.
- Recovery nonce is strictly increasing.
- Every accepted state transition is deterministic and committed in the state root.

## G. Explorer tests

- Show registered guardian slots as public keys or shortened fingerprints.
- Show guardian epoch and recovery nonce.
- Show pending request ID, proposed new key fingerprint, approval count (e.g. 2/3), start height, ready height, and expiry height.
- Show cancellation/finalization status.
- Do not display private keys, seed phrases, raw recovery shares, or secrets.
- Tampered JSON, duplicate approvals, wrong signatures, stale epochs, and early finalization must be shown as invalid with a clear reason.
- Browser replay and C++ replay must agree on every tested recovery block.

## H. Test execution evidence required

A PR must include actual logs for:
- CMake configure;
- full build;
- `ctest --output-on-failure`;
- ASan/UBSan build and tests;
- deterministic two-node replay;
- explorer cross-check;
- fuzz run duration and corpus summary.

A green GitHub workflow that only prints messages is not test evidence.
