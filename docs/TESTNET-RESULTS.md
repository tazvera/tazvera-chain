# First successful local run, 27 November 2026

This document records actual local-run figures supplied by the user. The figures and terminal/API messages below are preserved as supplied.

**Security:** Only public account addresses are included. No private key or seed is recorded or will ever be recorded.

## Chain identity

- Genesis pin: `5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63`
- `chain_id`: `tazvera-local-prototype-v1`
- Protocol: `3`

## Accounts

Public addresses and genesis balances only:

- k1: `c68b2d0cb47a57e30834fc235b8422bfdc09d728` — genesis balance `1,000,000`
- k2: `f46c08954f95a092e225d33d9b3ee379c05c14b7` — genesis balance `0`

## Producer node

Command:

```sh
taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --serve 7001 --listen-on 0.0.0.0 --produce-every 5
```

- Produced 131 blocks automatically.
- Warning printed: "listening on 0.0.0.0 makes this node reachable beyond this machine; is not encrypted"
- Note printed: "this chain declares no producers, so blocks are unsigned"

## Follower node

Command:

```sh
taznode --chain n2.json --expect-genesis <pin> --peer 7001
```

- Height `0 → 28`, "accepted 28 block(s)"
- "peer is anonymous (no identity proof)"

## Token transfer

Command:

```sh
taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --transfer f46c08954f95a092e225d33d9b3ee379c05c14b7:1000
```

- Block height 131 was created with one transaction.
- API result: k1 = balance 998000, nonce 1 | k2 = balance 1000, nonce 0
- In other words, 1000 sent + 1000 fee = 2000 deducted ✓

## What this demonstrates

1. Blocks are produced automatically.
2. A second node synchronizes.
3. The chain is saved to a file (the same height remains after closing and reopening).
4. Balance/nonce can be read through the API.
5. Token transfer, fee deduction, and nonce increment all work as observed.

## What this does not demonstrate

- Both nodes ran on the same machine (through loopback via 0.0.0.0).
- Two separate machines, the other side of a firewall, and a different OS remain untested.
- The peer did not prove its identity (anonymous).
- Communication was unencrypted (no TLS).

This is a local prototype run; it does not establish cross-machine networking, peer authentication, TLS, consensus/finality, or production readiness.

## How to reproduce

This follows the same producer → follower → transfer order. It assumes the local test chain and its matching test key file already exist. Replace `<pin>` with the genesis pin above. Do not share or add the private key file to the docs.

1. In the producer terminal:

   ```sh
   taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --serve 7001 --listen-on 0.0.0.0 --produce-every 5
   ```

2. In a second terminal on the same machine, start the follower:

   ```sh
   taznode --chain n2.json --expect-genesis <pin> --peer 7001
   ```

3. Run the transfer against the producer chain:

   ```sh
   taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --transfer f46c08954f95a092e225d33d9b3ee379c05c14b7:1000
   ```

4. Read both accounts' balance and nonce through the API. The expected result for this recorded run is: k1 balance `998000`, nonce `1`; k2 balance `1000`, nonce `0`. This is a reproduction guide, not a guarantee for every future run.

**Security note:** This run logged the `0.0.0.0` listener and unsigned-block warnings. Do not run it on a public network; it had neither TLS nor peer identity proof.

---

## Additional local test results — 27 November 2026

The following results are a separate, later run recorded for 27 November 2026:

- Two nodes ran on **the same machine**.
- The producer created **133 blocks**.
- The second node synchronized **28 blocks**.
- One token transfer completed. The resulting balances were **998000** for the sender and **1000** for the recipient.
- An asset was registered in **block 132**.
- Ownership of that asset was transferred in **block 133**.

**Important network limitation:** Both nodes were on the same machine. A cross-machine connection is still not possible with the current peer-address support; this run does not demonstrate cross-machine networking or sync.

These are reported results for this local run only. They do not prove production readiness, consensus/finality, peer authentication, TLS, or cross-machine operation.
