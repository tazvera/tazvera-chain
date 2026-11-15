# Core v8 branch notes

- load_chain_json + strict parse + full accept_block replay
- genesis_balances always stores both accounts (incl. zero) for round-trip
- format v1/v2/v3 accepted; protocol_version and params must match binary
- no private keys in JSON
- do not merge without green CI
