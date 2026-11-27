# Guide to Running TAZVERA Nodes on Two Machines

This guide is for preparing a testnet on a trusted LAN/Wi-Fi in the same home. **Peer connections between two separate machines are not currently available**: `--peer` connects only to `127.0.0.1`; an option to provide a remote address has not been implemented. Until then, run both nodes on **the same machine**. Do not open a port to the public internet.

## Current limitation

- `--listen-on ADDRESS` exists and is for binding to machine A's LAN interface with `--serve PORT`.
- `--peer` currently accepts only a localhost port; `--peer A_LAN_IP:PORT` is not a working remote-peer command yet.
- Therefore, the A/B LAN commands below are **expected syntax for a future remote-peer option**, not instructions for a test that works today. Until remote peer address support is merged and tested, use only the localhost test.

## Find the LAN address

On Windows machine A, run `ipconfig` in Command Prompt and note the Wi-Fi adapter's IPv4 Address (for example, `192.168.1.25`). On Linux, run `ip addr` and use the `inet` address of the connected Wi-Fi interface. Do not use `127.0.0.1`, a VPN address, or a virtual adapter address. Both machines must be on the same trusted LAN; guest Wi-Fi/client isolation may block connections.

## Expected two-machine commands — after remote `--peer` support is available

Use compatible binaries built from the same commit and the same genesis pin on both machines. Replace `<pin>` and the IP placeholders with real values.

**Machine A — server:**

```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001 --listen-on <A LAN address>
```

Keep A's terminal open. Use the LAN address of A's connected LAN interface. Port 7001 must be available.

**Machine B — peer:**

```sh
taznode --chain b.json --expect-genesis <pin> --peer <A LAN address>:7001
```

**Important:** The address-taking option for `--peer` to connect from B to A has not been implemented yet. Do not run the B command above until remote-peer support is merged, appears in `--help`, and is tested. Until then, run both nodes on the same machine; that test uses server `--serve 7001` and peer `--peer 7001`.

## LAN checks after remote peer support is available

1. Ping A's correct LAN IPv4 address from B. If ping fails, check Wi-Fi/SSID, guest network, subnet, VPN, and router client/AP isolation; ping being blocked alone does not prove TCP failure.
2. Run the node on A with `--serve 7001 --listen-on <A LAN address>` and confirm the log shows a successful bind/listen.
3. In A's Windows Firewall, allow inbound TCP 7001 only on the trusted Private network. Do not turn off the firewall entirely; remove the temporary rule after testing.
4. In B's command, provide A's correct LAN IP and the same port. Do not configure internet port forwarding on the router.
5. If the connection fails, check in order: is the node running? Is the LAN IP correct? Is the port the same and available? Is the firewall rule on the correct profile/port? Are the binaries from the same compatible commit? Then check both logs.

## Currently supported test on one machine

Open two terminals and use separate copies of the chain.

Server terminal:
```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001
```

Peer terminal:
```sh
taznode --chain b.json --expect-genesis <pin> --peer 7001
```

Check for successful connection/sync logs in both terminals. B's height will increase only when A has blocks beyond B's. Both files must have the same genesis pin. The exact executable path depends on your build; on Windows, normally use `build/taznode.exe`.

## Security

This is a development test: balances/transactions are test data only, with no economic value and no payment service. Do not share private keys/seeds. TLS is not wired in yet; keep plaintext traffic on a trusted LAN. Do not expose the node on public Wi-Fi or the public internet.
