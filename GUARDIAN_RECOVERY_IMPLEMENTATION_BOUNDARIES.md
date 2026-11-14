# Guardian Recovery — Implementation Boundaries

This file prevents a prototype from being presented as a finished wallet recovery feature.

## What this first design package does

- Defines a 2-of-3 guardian approval policy.
- Defines delayed recovery and an independent owner veto key.
- Defines key rotation while preserving a stable account ID.
- Defines replay protection, guardian epochs, nonces, canonical state, and test requirements.

## What it does not do

- It does not add C++ recovery transactions.
- It does not change account authentication.
- It does not recover an old private key.
- It does not create or store guardian shares.
- It does not alter the existing Shardeum Mainnet TZV token.
- It does not make the prototype suitable for real funds.

## Why implementation is gated

The current prototype binds an account address to the public key in each transaction. A recovery feature that changes the public key must first change the account model to a stable account identifier plus an active authorization key. Implementing only a recovery transaction without that migration would either fail to recover the account or create an unsafe bypass.

The next coding phase should be a separately reviewed protocol migration, not a quick patch to `State::apply`.
