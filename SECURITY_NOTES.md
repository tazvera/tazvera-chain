# Security notes for reviewers

- The previous repository implementation contained an Ed25519 verification placeholder that accepted every signature. This package implements actual OpenSSL EVP Ed25519 verification.
- Never commit private seeds, mnemonics, production keys, API tokens, or wallet backups.
- Demo executable creates a fresh random local key and prints only its address. It does not save or export the secret key.
- `PrivKey` is a 32-byte Ed25519 seed. Production key storage, secure memory handling, backup/recovery, and signing UX are not implemented.
- The current block chain is an in-memory single-process state machine. It has no peer agreement or consensus and must not be described as a functioning decentralized network.
- Before adding networking, define a versioned wire protocol and fuzz every parser. Before consensus, specify safety/liveness assumptions and test Byzantine and partition scenarios.
