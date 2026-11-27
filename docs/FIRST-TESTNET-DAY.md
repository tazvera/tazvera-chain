# TAZVERA testnet: first-day checklist

This is a local development test, not a public testnet launch or payment service. Balances/transactions are test data only; do not share private keys/seeds. **Current limitation:** `--listen-on` is available, but `--peer` currently connects only to `127.0.0.1`. The `--peer <address>:<port>` option for connecting from B to A's LAN address has not been implemented yet. Therefore, perform the two-machine sync stage only after that option has been merged and tested. Until then, run two nodes on one machine and check localhost sync.

## 1. Build the binary on both machines

- Check out the same repository commit on A and B; record the commit SHA.
- Install dependencies, configure, build, and run tests on both. Both builds must succeed.
- Both binaries must come from that same commit. If an old binary is running, stop it and build the new one.

## 2. Create keys, create the chain, and match the pin

- Create test-only node keys on A and B. Note the public addresses; do not send private keys/seeds to anyone.
- Decide the order of public addresses and balances used in genesis. Use **exactly the same addresses, balances, order, and options** on A and B.
- Create separate, new chain files on both machines: `a.json` on A and `b.json` on B. Run the same genesis command/options.
- Record the complete genesis commitment/pin from both outputs. **The pin must be byte-for-byte identical.** If it differs, do not continue; check the inputs and create new disposable chain files.
- Give both nodes this pin with `--expect-genesis <pin>`.

## 3. Run A as the server and check its LAN address

- On A, use `ipconfig` (Windows) or `ip addr` (Linux) to find the IPv4 address of the connected LAN/Wi-Fi interface. Do not use `127.0.0.1`, a VPN, or a virtual adapter address.
- Both machines must be on the same trusted LAN/Wi-Fi; do not use guest Wi-Fi/client isolation.
- Run this on A only after remote-peer address support has been merged and tested:

```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001 --listen-on <A LAN address>
```

- Keep A's terminal open and check the log for a successful bind/listen. Confirm that the LAN address and port 7001 are correct.

## 4. Connect from B and check sync/height

The remote `--peer <address>:<port>` option is not available yet. Run this command on B only after that option has been merged and tested:

```sh
taznode --chain b.json --expect-genesis <pin> --peer <A LAN address>:7001
```

Check B's logs for connection/sync. Height will increase only if A has blocks beyond B's current height; if A has no new blocks, an unchanged height is not automatically a failure. The genesis pin must match on both sides.

**Current fallback:** The `--peer` address option for connecting from B to A's LAN address has not been implemented yet. Until then, run both nodes on the same computer: A/server `--serve 7001 --listen-on 127.0.0.1`, B/peer `--peer 7001`; use separate copies of the chain files.

## 5. If the connection fails, check one item at a time

1. **Is A running?** Check the server process and bind/listen log.
2. **Is the address correct?** Use A's connected LAN IPv4 address; B's command must use that address and port 7001.
3. **Firewall?** On A, allow only inbound TCP 7001 for the trusted Private LAN. Do not disable the firewall entirely; remove the temporary rule after testing.
4. **Port busy?** If you see a bind error/address already in use, stop the old node or choose an unused port; use the same new port on A and B.
5. **Old binary?** Compare the commit SHA and build path on A/B; rebuild both from the same commit.
6. **Network isolation?** Both must be on the same trusted Wi-Fi/LAN; check guest networks, VPN, router client/AP isolation, and third-party firewalls.
7. **Pin mismatch?** Compare both genesis pins and genesis inputs. Do not continue syncing with a wrong pin.
8. **Logs/height:** Record the complete relevant logs from both terminals and both heights. Ping success alone is not proof of a TCP connection.

## 6. Keep a test record

- Date/time and who performed the test/on which machines.
- OS on both machines and the node binary commit SHA; build/test result.
- Genesis pin on A/B (complete or in a secure record) and whether they matched.
- A's LAN address (keep the record private), port used, and the exact commands on A/B.
- Whether the connection succeeded; A and B heights before/after sync; relevant log lines.
- Any temporary firewall changes and whether they were removed afterward.
- If it failed, the exact error, the stage where it occurred, and which checks above were performed.

## 7. Stop nodes safely

- Press **Ctrl+C** in both node terminals and wait for the processes to stop.
- If a process does not stop, check the terminal/log first; force-kill only as a last resort.
- **Never manually edit, truncate, or "repair" a chain file.** If something goes wrong, stop the node and preserve copies of the logs and chain file. Create a new chain with a new filename only after confirming the data is disposable test data.
- Do not share private keys/seeds. After testing, remove temporary firewall rules and do not leave any port forwarding open to the public internet.

This test is a limited check of LAN connectivity/sync; it does not prove production readiness, consensus safety, TLS, or public testnet readiness.
