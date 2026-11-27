# Security status before testnet

This document describes the current prototype, not a production or mainnet security guarantee. Re-check it against the exact release commit before running a public testnet.

## What the node currently provides

- **Signed hello / peer identity:** the authenticated handshake proves control of a key and binds the proof to the connection transcript. This identifies a key; it does not by itself make that peer trustworthy.
- **Peer allowlist:** when identity is required, configured peer addresses can restrict which identities are accepted. An allowlist is only as trustworthy as the keys and configuration supplied by the operator.
- **Bounded message framing:** network messages have explicit size limits. This limits the size of an individual framed message; it is not a connection or request rate limit.
- **Writer lock:** the node protects its chain-file writer against concurrent writers. This does not protect against a hostile peer or a compromised machine.
- **TLS groundwork exists, but TLS is not wired into the node transport.** The network connection is plaintext today.

## What it does not provide

- **No TLS encryption:** network traffic can be read by anyone able to observe the path. Do not send secrets over this connection.
- **No connection/request rate limit (F4):** peer-count limits are not rate limiting. A peer can still consume time and resources within the current protocol limits.
- **No independent producer availability (F7):** if the producer is stopped, the chain stops advancing. This prototype does not provide production-grade consensus or fault-tolerant block production.
- **No real-world payment path:** local prototype transactions and balances are test data only; they do not represent funds or a payment service. The prototype has no production payment path.
- **The HTTP API is loopback-only:** it binds to `127.0.0.1`, not to the LAN or public interfaces.

## Listening on a network interface

The node listener defaults to `127.0.0.1`. Use `--listen-on <IPv4-address>` with `--serve <port>` or `--listen <port>` to select the listener address. Without it, the listener remains loopback-only. `--listen-on` does not change the `--peer` destination behavior.

Passing `--listen-on 0.0.0.0` means **listen on every IPv4 network interface on that machine**. Other devices on the same network—and potentially devices beyond it, depending on router/firewall rules—may be able to connect. The connection is unencrypted because TLS is not connected. Do not use this setting on a public network or expose the port to the internet.

An explicit LAN address is narrower than `0.0.0.0`, but it is still plaintext and should only be used on a network you trust.

## Testnet operating advice

**Run nodes only on a trusted home Wi-Fi network for local testing. Do not run them on public Wi-Fi, shared networks, or internet-facing hosts.** Keep the firewall enabled and allow only the required port on the private network profile. Use test-only keys and data; do not share private keys or secrets.

This is an experimental prototype. It has not been established as safe for public-network or production use.
