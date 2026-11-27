# TAZVERA Chain HTTP API

The optional HTTP API is **read-only** and intended for local tools and wallet development. It is disabled unless the node is started with `--api <port>`.

## Start and network safety

```sh
taznode --chain <file> --expect-genesis <64-hex-genesis> --api 8765
```

- The server binds only to **127.0.0.1**. It is not a LAN or public-network API.
- Requests with a supplied `Host` header are accepted only for `127.0.0.1:<port>` or `localhost:<port>`.
- CORS is **disabled by default**. Browser clients receive no `Access-Control-Allow-Origin` header unless an exact origin is allowlisted.
- To allow a browser origin, add one or more `--api-allow-origin <origin>` arguments. Example:

```sh
taznode --chain <file> --expect-genesis <64-hex-genesis> --api 8765 --api-allow-origin https://app.tazvera.com
```

Origins must be exact `http://` or `https://` origins; `*` is rejected. CORS does not make the API remotely reachable: the listener remains bound to loopback.

## Endpoints

All data endpoints use `GET` and return JSON.

| Endpoint | Success response |
|---|---|
| `GET /status` | `chain_id`, numeric `protocol_version`, string `height`, and `genesis` commitment |
| `GET /balance/{address}` | `balance` and `nonce` for a 40-hex-character address; both are strings |
| `GET /assets/{address}` | JSON array of assets currently owned by that address |
| `GET /asset/{asset_id}` | One asset record, or 404 if it does not exist |
| `GET /assets` | Registry-wide, cursor-paginated asset listing; see below |
| `GET /block/{height}` | One block in the chain's block JSON format |

Addresses are exactly 40 hexadecimal characters. Asset IDs and document hashes are exactly 64 hexadecimal characters. Hexadecimal input accepts upper- or lowercase.

### Registry-wide asset listing

`GET /assets` returns an object:

```json
{
  "assets": [
    {
      "asset_id": "64-hex-characters",
      "asset_kind": 1,
      "doc_hash": "64-hex-characters",
      "created_at": "123"
    }
  ],
  "next_after": "last-returned-asset-id-or-null"
}
```

- Default page size: 100.
- `limit` is optional and must be an integer from 1 through 100.
- `after` is optional and must be a 64-character hexadecimal asset ID. The next page starts **after** that ID (exclusive).
- The registry is returned in ascending asset-ID order.
- `next_after` is the last asset ID in this page when another page exists; otherwise it is JSON `null`.
- Example: `GET /assets?after=<asset_id>&limit=50`.

Unknown, duplicate, malformed, or out-of-range query parameters return 400.

## Integer representation

**Wallet developers: every 64-bit integer value is encoded as a JSON string**, not a JSON number. This avoids precision loss in JavaScript and other clients whose number type cannot exactly represent every 64-bit integer.

Examples include `height`, `balance`, `nonce`, `created_at`, and the 64-bit integer fields in block responses. `protocol_version` and the small `asset_kind` enum are JSON numbers.

## HTTP status codes

| Status | Meaning |
|---|---|
| `200 OK` | Request succeeded |
| `204 No Content` | Valid `OPTIONS` preflight; no response body |
| `400 Bad Request` | Malformed request/headers, invalid address or ID, invalid query parameters, or disallowed/duplicate `Host` |
| `404 Not Found` | Unknown endpoint, asset ID, or block height |
| `405 Method Not Allowed` | Method other than `GET` or `OPTIONS`; API is read-only |

Error responses use JSON: `{"error":"description"}`. The API does not expose write endpoints.

## Wallet integration

Wallets can use the current read-only API to display:

- **Balance and nonce:** `GET /balance/{address}` returns the balance and next nonce for the supplied 40-character address. Both 64-bit values are JSON strings.
- **Assets owned by an address:** `GET /assets/{address}` returns the assets owned by that address.
- **Registry-wide assets:** `GET /assets?limit=100` returns registered assets in pages. To request the next page, pass the previous response's `next_after` value: `GET /assets?after=<next_after>&limit=100`.

### Transferring asset ownership

Changing ownership requires creating a `TransferAsset` transaction and signing it with the current owner's authority. **This can currently be done only through `taznode`; wallet transaction creation and submission are not available.**

**Important:** This HTTP API is read-only. It has no write endpoint. A wallet can read balances and assets through this API, but cannot submit a transfer or any other transaction through it.

## Known limitations

- The HTTP API is read-only; it has no write or transaction-submission endpoint.
- CORS is disabled by default.
- The API listens only on `127.0.0.1`; it is not available over a LAN or public network.
- All 64-bit integer values are returned as strings rather than JSON numbers.
