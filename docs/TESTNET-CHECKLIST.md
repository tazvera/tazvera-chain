# Pre-Testnet Checklist

This checklist determines whether starting the testnet is authorized. Check an item only after its evidence has been recorded. A successful build or running on one machine alone is not sufficient.

## 1. Code and security

- [ ] The behavior of `--listen-on <IPv4-address>` has been checked. It sets the listener address with `--serve <port>` or `--listen <port>`; the default is `127.0.0.1`.
- [ ] `--produce-every` has not been implemented yet; do not treat it as an available feature or tested behavior. The CLI's `--every <seconds>` is only the `--peer` reconnect interval, not the block-production interval.
- [ ] **F7 is open:** producer availability/slot skipping has not been implemented or verified. The chain may stop if the producer stops; do not treat this issue as closed.
- [ ] **F4 is open:** bounded frame/message and sync batch/time/byte limits exist, but connection/request rate limiting does not. Do not mark this complete; only consider it complete after the rate-limit/cooldown design and abuse tests are implemented.
- [ ] All new and existing network tests have been run; no test has been removed, skipped, or changed to use a weaker assertion.
- [ ] Build and test results are recorded for Linux and Windows; sanitizer/fuzz tests pass where applicable.
- [ ] The help/usage and error behavior for `--listen-on` are correct. `--produce-every` has not been implemented yet; do not claim any available CLI behavior for it.

## 2. Testing on two separate machines

- [ ] Two separate computers on the same home Wi-Fi/LAN can connect using each other's LAN addresses.
- [ ] Both nodes have the same genesis commitment; sync is rejected for an incorrect genesis.
- [ ] Both nodes complete hello/authentication and check the expected identity/chain information of the other.
- [ ] After a block is produced on one node, the other syncs it; both chain files and heights have been checked.
- [ ] Disconnect/reconnect, slow peer, invalid message, and sync interruption tests have been run; the node does not hang or corrupt the chain.
- [ ] This LAN test was performed **without TLS**, and its limitation is understood; it is not evidence of a secure connection over the public internet.

## 3. Run instructions

- [ ] A second person followed `docs/RUN-A-NODE.md` from beginning to end without verbal help from the author and successfully ran a node.
- [ ] That person also ran two nodes and completed sync and shutdown.
- [ ] The flags, ports, file names, and expected output used in the instructions match the current code.
- [ ] Windows and Linux instructions have been verified separately where supported; any unverified platform is clearly identified.

## 4. Testnet limitations — read before starting

- [ ] **Test data only:** balances/transactions have no declared economic value and there is no real-world payment path; do not treat them as payments or funds.
- [ ] The local prototype transfer is not presented as a real payment or wallet/payment service.
- [ ] **No TLS:** transport is not encrypted. Restrict it to controlled, trusted LAN testing; do not run it on public Wi-Fi or the internet.
- [ ] **No consensus:** production-grade consensus/finality between nodes does not exist. Seeing the same chain or completing sync does not guarantee a canonical chain/finality.
- [ ] The testnet is not presented as a production, secure, decentralized, or economically usable network.

## 5. If something goes wrong

- [ ] **Node stopped:** Save the complete terminal error/log; stop the process normally and check by restarting it with the same configuration. If it repeatedly crashes, stop the testnet and file an issue.
- [ ] **Sync did not happen:** Check both nodes' genesis commitments, listen address/port, LAN connectivity, firewall, peer command, and logs. Do not combine chain files until a different genesis or incorrect port has been corrected.
- [ ] **Chain file corrupted:** Stop the node and preserve a copy of the damaged file. Create a new test chain only after confirming that the data is disposable; do not silently overwrite/delete the damaged file. Keep the required logs and file separately.
- [ ] **Unexpected block/height or protocol error:** Stop the testnet, preserve both nodes' logs and chain-file copies, and do not treat it as normal behavior before understanding the cause.
- [ ] Do not mark the checklist complete until the cause and resolution of the problem have been recorded.

## 6. Final authorization

- [ ] Evidence for all applicable items above is recorded, and there are no unresolved critical/high security issues.
- [ ] The responsible reviewer has explicitly authorized starting the testnet.
