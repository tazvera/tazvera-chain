# Security notes for reviewers

- The previous repository implementation contained an Ed25519 verification placeholder that accepted every signature. This package implements actual OpenSSL EVP Ed25519 verification.
- Never commit private seeds, mnemonics, production keys, API tokens, or wallet backups.
- Demo executable creates a fresh random local key and prints only its address. It does not save or export the secret key.
- `PrivKey` is a 32-byte Ed25519 seed. Production key storage, secure memory handling, backup/recovery, and signing UX are not implemented.
- The current block chain is an in-memory single-process state machine. It has no peer agreement or consensus and must not be described as a functioning decentralized network.
- Before adding networking, define a versioned wire protocol and fuzz every parser. Before consensus, specify safety/liveness assumptions and test Byzantine and partition scenarios.
- v2: key seeds are wiped with `OPENSSL_cleanse` where the core owns them, but callers that copy a `PrivKey` must wipe their own copies. Swap/core dumps and secure memory (mlock) are still not handled.
- v2: `max_timestamp` is supplied by the caller. Choosing the allowed clock drift is a consensus-rule decision still to be specified.
- v2: the state root is a flat hash over all accounts (O(n) per block). A Merkle tree is planned so light clients can verify balances with proofs.
- v5: the safety rules help most against mistakes, scams and rushed decisions, where the owner still holds the key and notices in time. Against a stolen key they only limit the damage: a thief can still move up to the daily limit and anything under the hold threshold, and can see the same cancel button the owner has. Recovering a lost key is handled by guardian recovery (v6).
- v5: `CancelHeld` restores funds but does not give back the used part of the daily allowance (conservative; documented in the tests).
- v5: a hold longer than a few days is deliberately impossible (`MAX_HOLD_BLOCKS`), so funds cannot be locked forever.
- v5: held funds still belong to the sender until release; a receiver must not treat a held transfer as received.
- v6/v7: guardian recovery is a delayed, vote-based move of funds to a new address. Without a veto key, anyone with the account's key may cancel it, so a thief can block the guardians. With a veto key (v7), only that key may cancel.
- v6: any `threshold` guardians acting together can take the funds after the delay; this cannot be prevented cryptographically. Do not describe it as "guardians can never steal".
- v6: guardian changes wait SAFETY_DELAY_BLOCKS so a stolen key cannot swap in the thief's helpers at once; RECOVERY_DELAY_BLOCKS is shorter on purpose.
- v6: votes are stored in consensus state (map guardian -> new address) and are part of the state root; the list of guardians is canonical (sorted, unique).
- v7: the veto key can stop every recovery of its account. It is a trade-off, not a free lunch: keep it offline and separate. Its account pays the cancel fee.
- v7: v6 chain files do not verify under v7 (signed bytes and state root changed). Prototype only.
- v8.1: persistence loads by full replay (every block through `accept_block`) with a strict parser and size limits; the address of a zero-balance genesis account is not authenticated (see README). Windows saving has no `fsync` and is untested.
- v8.1: a thief holding the key can add his own veto key to an account that has guardians but no veto, and guardians that are more than about 200 blocks late then lose permanently (test `test_thief_adds_veto_late`). Set the veto key together with the guardians.
- v8.1: `max_timestamp` still defaults to "no limit" in `Chain`; the caller must supply a clock-drift bound. Blocks carry no producer signature (known, consensus work is not started).

- v8.2: PROTOCOL_VERSION = 2. Protocol-1 chain files are refused with an explicit message.
- v8.2: the genesis commitment (domain, chain id, protocol, both accounts sorted) is stored as block 0's `previous_hash`, so it is inside the block hash chain; zero-balance genesis addresses are authenticated (supersedes the v8.1 note above). A genesis-only file still needs an out-of-band comparison.
- v8.2: a guardian/veto change that comes due while a recovery has reached threshold is discarded, and a veto postpones an active recovery only once (300 blocks). This replaces the v8.1 "thief veto locks forever" behaviour. Remaining delay attacks are listed in README "Known limitations".
- v8.2: Windows save now uses FlushFileBuffers + MoveFileExW; compiled/tested only by CI if it runs there.
- v8.2: still not production-ready: no P2P, no consensus, no professional audit, placeholder constants.

- v8.3: the genesis commitment is checked against the file and against block 0, which proves internal consistency only. A caller that knows which chain it meant to join should set `LoadOptions::expected_genesis_commitment`; no CLI or node does that yet.
- v8.3: the Windows replace is retried for sharing/access violations; without the retry the concurrent-save test failed on the Windows runner (it was the runner's antivirus, not the code).
- v8.3: `.gitattributes` pins `eol=lf`. A CRLF checkout makes the demo JSON differ byte-for-byte from `to_json`, which the persistence test treats as a failure on purpose.
- v8.3: `JVal` used `std::vector<std::pair<std::string, JVal>>`, which clang rejects (incomplete type at instantiation). Latent since v8.1; found by the clang-built fuzz job. Object members now carry a `key` field.
- v8.7: a stale `<path>.tmp.*` file older than an hour is deleted by the next save. Nothing else is touched; failures are ignored.
- v8.7: `LoadOptions::max_blocks` bounds a load at 200 000 blocks, on top of `max_nodes` and `max_file_bytes`. The DOM parser still holds the whole file in memory, so the real bound is `max_nodes`.
- v8.7: the fuzz targets are a smoke test (60 s each), not a fuzzing campaign, and the corpus is not committed.
- v8.7: with no veto key, the owner key can cancel a recovery repeatedly. Pinned by `test_owner_key_can_cancel_without_veto`. Guardian recovery is for a LOST key; against a STOLEN key it limits damage, it does not restore control.

- v8.8: `load_chain_json` converts the path once and uses it for `status`, `file_size` and `ifstream`. Before this, saving under a non-ASCII directory worked but loading it failed on Windows (narrow `std::string` went through the ANSI code page).
- v8.8: `LoadOptions::require_genesis_pin` is off by default. It exists so the future node can refuse to load a chain whose genesis it cannot identify. Turning it on changes nothing else.
- v8.8: the duplicate copies of `persist.cpp`, `persist.hpp`, the workflow files and the tests that sat in the repository root are removed. They were never compiled, but two files with the same name and different content is exactly how a security fix gets lost.
- v8.8: guardian and veto limits are now stated together in one README section, so a reader does not have to assemble them from five places.
