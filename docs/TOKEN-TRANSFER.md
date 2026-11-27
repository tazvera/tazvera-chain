# Sending and receiving tokens

## What is a token?

A token is a number recorded in the chain's ledger. It is not real money by itself and has no guaranteed cash value. This document is for testing the current TAZVERA prototype.

## How to send tokens today

Tokens are currently sent through the command line. You need the sender's key file and the correct chain's genesis pin. In the command below, replace `<recipient-address>` with the recipient's complete 40-character hexadecimal address and `<pin>` with the trusted genesis pin.

```sh
taznode --chain c1.json --expect-genesis <pin> --key-file a.key --transfer <recipient-address>:1000
```

This example sends 1000 token units. The current CLI creates one block for a transfer and then stops. This is a prototype command, not a public payment service.

## Why is a 1000-unit fee charged?

A fee of 1000 units is charged for every transaction. The sender's account is debited by both the amount sent **and** the 1000-unit fee. The fee is burned; the recipient does not receive it. Its purpose is to help protect the network against useless or spam transactions. This is the current prototype fee, not a final economic rule.

Example: to send 5000 units, the balance must be at least 6000 units.

## What happens after sending?

The command places the transaction in a block. Another node synchronizes the block, verifies it, and accepts it into its own chain. This is a prototype; block production by itself is not legal or economic confirmation of a payment.

## How to check a balance

Enable the read-only API on the node:

```sh
taznode --chain c1.json --expect-genesis <pin> --api 8080
```

Then make this GET request from a browser or HTTP client:

```http
GET /balance/<address>
```

Example URL: `http://127.0.0.1:8080/balance/<address>`

The response contains the balance and next nonce:

```json
{"balance":"10000","nonce":"0"}
```

Here, `10000` and `0` are examples only. The current API is read-only and loopback-only; it is not an endpoint for sending transactions.

## Common errors

- **Invalid address:** The address must be exactly 40 hexadecimal characters. The API returns `address must be exactly 40 hexadecimal characters` for an invalid address. An invalid/incomplete address in the transfer command produces a usage error.
- **Insufficient balance including the fee:** `insufficient balance`. The balance must cover both the amount being sent and the 1000-unit fee.
- **Wrong nonce:** `bad nonce`. The sender's nonce must equal the next nonce recorded on the chain. Check the balance API's `nonce`.
- **Wrong chain:** `wrong chain ID`. The transaction and node must belong to the same chain; use the correct chain file and trusted genesis pin.

## Important warning

Check the address twice before sending. A token sent to the wrong address cannot be recovered—treat this as you would a real chain. Never give anyone your private key file.
