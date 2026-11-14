# TAZVERA Guardian Recovery — Design v1 (Proposal)

**Status:** Design proposal only. Not implemented. Not safe for real funds.
**Target:** `tazvera-chain` local prototype.
**Policy:** 3 guardians, threshold 2-of-3, delayed recovery, independent owner veto.

## 1. Goal

Allow an account owner who loses their everyday signing key to rotate control to a new key with help from at least two of three previously registered guardians.

The chain must never store private keys, seed phrases, raw Shamir shares, recovery codes, or encrypted wallet backups. Guardians approve a narrowly-scoped recovery request; they do not receive the owner's private key.

## 2. Critical architecture prerequisite

The current prototype derives an address from a public key and validates transactions by checking that the transaction public key maps to `tx.from`. That model does not support changing the controlling public key while keeping the same account address.

Therefore recovery must **not** be bolted onto the current transaction code as a simple `new_pubkey` field.

Before implementing consensus rules, introduce a stable account identifier (Account ID) that is independent of the current signing public key:

- `account_id`: permanent 20-byte or 32-byte identifier.
- `active_spend_key`: current Ed25519 public key authorized to spend.
- `recovery_veto_key`: independent public key, kept offline by the owner, used only to veto a pending recovery.
- `guardians[3]`: three distinct Ed25519 public keys.
- `guardian_epoch`: incremented whenever the guardian set changes.
- `recovery_nonce`: monotonically increasing request counter.

This is a protocol/state-root migration and needs a versioned transaction format. It is not a wallet-only patch.

## 3. Registration

Account setup registers exactly three distinct guardian public keys and one independent recovery-veto public key.

Rules:

1. Owner signs the registration using the currently active spend key.
2. Owner also proves possession of the recovery-veto key by signing a domain-separated registration challenge.
3. A guardian key cannot equal the active spend key or veto key.
4. Duplicate guardian keys are rejected.
5. Guardian-set replacement requires the current spend key plus veto-key authorization and a delay. It must not be possible to silently replace guardians using one compromised key.
6. Registration changes increment `guardian_epoch`; old guardian approvals become invalid.

Guardian public keys are public metadata. Guardian private keys stay with the guardians.

## 4. Recovery request

A request contains:

- protocol version and chain ID;
- account ID;
- current guardian epoch;
- recovery nonce;
- new active spend public key;
- request expiry height;
- request identifier/hash;
- signatures/approvals from guardians.

The request is domain-separated, for example `TAZVERA-GUARDIAN-RECOVERY-V1`. Every field above is included in the signed bytes. The request ID is a hash of the canonical request body excluding signatures.

The request is rejected if:

- the account has no registered recovery policy;
- the guardian epoch or recovery nonce is stale;
- the new key is invalid, all-zero, or equals a guardian/veto key;
- the request is expired;
- the same request ID was already used;
- signatures are invalid or duplicate;
- fewer than two distinct registered guardians approve;
- the proposed new key equals the current active key.

Exactly two of the three registered guardians are sufficient. One guardian can never authorize recovery.

## 5. Delay and owner veto

After valid 2-of-3 approval, the chain records a pending recovery with a deterministic `ready_height = inclusion_height + RECOVERY_DELAY_BLOCKS`.

The delay is counted in blocks, not wall-clock time. `RECOVERY_DELAY_BLOCKS` is a prototype constant until block time and threat model are formally decided.

During the delay:

- the current active spend key may cancel the request;
- the independent recovery-veto key may also cancel it;
- cancellation is signed with a separate domain, includes account ID, request ID, guardian epoch, recovery nonce, and chain ID;
- cancellation is accepted only for the exact active pending request;
- a cancelled request ID can never be replayed;
- a new recovery request requires a new recovery nonce.

**Why an independent veto key:** if an attacker steals the everyday spending key, that attacker must not be able to veto the legitimate guardian recovery. The owner should keep the veto key offline and separate from the phone wallet. Losing both the spending key and veto key does not prevent guardian recovery; it only removes the owner's direct veto path.

A recovery-veto key must not be a normal guardian key.

## 6. Finalization

At or after `ready_height`, anyone may submit a finalize transaction referencing the exact pending request ID.

The state transition must atomically:

1. confirm the pending request still exists and is not cancelled/expired;
2. confirm its guardian epoch and recovery nonce still match;
3. replace `active_spend_key` with the approved new public key;
4. increment the account authorization epoch and recovery nonce;
5. clear the pending request;
6. invalidate all approvals and signatures bound to the old authorization epoch.

The account ID and balances remain unchanged. The old spend key must no longer authorize transfers, safety changes, new recovery requests, or guardian changes.

The recovery-veto key remains unchanged unless separately rotated through a protected procedure.

## 7. Guardian collusion and limits

A 2-of-3 policy intentionally trusts any two guardians acting together. No cryptographic protocol can make a 2-of-3 threshold safe against two malicious or compromised guardians if those two are authorized to approve recovery.

Mitigations:
- choose independent guardians on separate devices/accounts;
- do not let one person control two guardian keys;
- notify the owner and guardians when a request begins (wallet/off-chain notification is not a consensus guarantee);
- enforce a meaningful delay;
- preserve the independent veto key;
- support guardian rotation with a delay and strong authorization;
- show the exact new public key and account ID in every approval UI.

Do not describe this as “two guardians can never steal the account.” The honest claim is “two of three registered guardian keys can authorize a delayed key rotation; the owner has an independent veto key during the delay.”

## 8. State machine

States:
- `Unregistered`
- `ActiveNoPending`
- `RecoveryPending`
- `RecoveryCancelled` (terminal event/history; no active pending request)
- `RecoveryFinalized` (terminal event/history; new key active)

Allowed transitions:
- `Unregistered -> ActiveNoPending`: owner registers guardians and veto key.
- `ActiveNoPending -> RecoveryPending`: valid request + 2 distinct guardian approvals.
- `RecoveryPending -> ActiveNoPending`: active spend key or veto key cancels.
- `RecoveryPending -> ActiveNoPending`: request expires before finalization.
- `RecoveryPending -> ActiveNoPending`: finalize after ready height; new key becomes active.

No transition may be triggered by only one guardian.

## 9. Consensus and canonical encoding

All recovery fields must be included in deterministic state serialization and state-root calculation:
- recovery policy presence;
- guardian epoch;
- three guardian public keys in canonical order (or fixed slots with explicit slot IDs);
- veto public key;
- recovery nonce;
- pending flag;
- pending request ID;
- proposed new key;
- request/ready/expiry heights;
- approval bitmap (three bits) and approval signatures or a canonical commitment to them;
- cancellation/finalization status if history is consensus state.

Use fixed-width fields, explicit domain separators, deterministic byte order, and checked arithmetic for height additions. Never rely on local time, UI state, map insertion order, or non-canonical JSON for consensus.

## 10. Out of scope for this chain layer

- Recovering or reconstructing the old private seed.
- Storing guardian shares or wallet backups on-chain.
- Email/SMS/phone-number identity verification.
- Push notifications.
- Wallet UI, guardian invitation UX, QR flows.
- Shamir Secret Sharing.

This design rotates the account's authorized key; it does not recover the lost private key. The wallet must generate a fresh key securely on the owner's new device and keep its seed locally. A separate wallet backup design may be considered later, but must be reviewed independently.

## 11. Release gates

Do not enable this for real-value accounts until:
- account-ID/key-rotation migration is reviewed;
- all state transitions are implemented and tested;
- cross-node deterministic replay tests pass;
- fuzzing and sanitizers pass;
- wallet signing/approval UX is reviewed;
- independent security review is complete;
- protocol versioning and migration are documented.
