# CHAIN REQUIREMENTS — असली चेन की मास्टर चेकलिस्ट
# Master checklist: what a real chain needs—and what it does not

आधार: वर्तमान main branch के source और उपलब्ध project docs की जाँच। मौजूद code path का अर्थ production readiness या सुरक्षा प्रमाणपत्र नहीं है।
Basis: current main source and project docs. An existing code path is not proof of production readiness or security.

स्थिति: ✅ मौजूद · ⚠️ आंशिक/सीमा सहित · ❌ बाकी
Status: implemented · partial/limited · not implemented

## A. जो होना ही चाहिए / Must-have

| # | आवश्यकता / Requirement | कोड में पुष्टि / Code-checked status |
|---:|---|---|
| 1 | दो मशीनों के नोड जुड़ें।<br><small>Nodes on two machines connect.</small> | ❌ --peer केवल 127.0.0.1:<port> को dial करता है। --listen-on से बाहरी inbound connection संभव है, पर outbound peer address नहीं।<br><small>Remote inbound listening exists; remote peer dialing does not.</small> |
| 2 | --produce-every से block अपने-आप बनें।<br><small>Automatic block production.</small> | ✅ --serve के साथ empty blocks बनते हैं; producer chain में सही key चाहिए।<br><small>Empty blocks are produced; producer chains require an authorized key.</small> |
| 3 | दूसरा नोड sync करे।<br><small>A second node synchronizes.</small> | ✅ Peer sync मौजूद है; दर्ज run local था, अलग मशीनों का test नहीं।<br><small>Peer sync exists; recorded test was local.</small> |
| 4 | Disk save और restart पर वही chain।<br><small>Persistence across restart.</small> | ✅ JSON save/load, replay और genesis pin checks मौजूद।<br><small>Persistence, replay, and genesis pin checks exist.</small> |
| 5 | Signed transfer, fee, nonce।<br><small>Signed transfers, fees, nonces.</small> | ✅ Transaction validation इनकी जाँच करती है।<br><small>Transaction validation checks them.</small> |
| 6 | Signed node identity hello।<br><small>Signed node identity hello.</small> | ✅ Identity proof और connection-bound nonce checks; enforcement --require-identity से।<br><small>Proof and nonce checks exist; enforcement is opt-in.</small> |
| 7 | Peer allowlist।<br><small>Peer allowlist.</small> | ✅ --allow-peer / --allow-peer-file, --require-identity के साथ।<br><small>Allowlisting is available with required identity.</small> |
| 8 | Replay protection और हर network का अपना chain_id।<br><small>Replay protection and distinct network IDs.</small> | ⚠️ Chain ID और genesis commitment messages/commitment में बंधे हैं; लेकिन CHAIN_ID अभी fixed compile-time constant है, per-network configurable नहीं।<br><small>Identity is genesis-bound, but the ID is not configurable per network.</small> |
| 9 | Deep fork साफ़ reject हो।<br><small>Explicitly reject deep forks.</small> | ✅ Unrepairable peer tip पर refusal/non-zero failure path है।<br><small>Unrepairable deep forks are refused.</small> |
| 10 | Fork reorg से सुलझे।<br><small>Resolve forks by reorganization.</small> | ❌ Common-ancestor search और multi-block atomic reorg नहीं; deep fork reject होता है।<br><small>No common-ancestor reorganization.</small> |
| 11 | Producer बंद हो तो chain रुके नहीं (F7)।<br><small>Chain continues if a producer stops.</small> | ❌ Producer failover/consensus takeover नहीं।<br><small>No producer failover or consensus takeover.</small> |
| 12 | Slow/hostile peer पूरे network को धीमा न करे (F4)।<br><small>One slow peer cannot stall the network.</small> | ❌ F4 connection-cost/rate-control work बाकी; timeouts इसे पूरा नहीं करते।<br><small>F4 remains unfinished; timeouts are not equivalent.</small> |
| 13 | Peer traffic TLS से encrypt हो।<br><small>Encrypt peer traffic with TLS.</small> | ❌ TLS context और TlsTransport adapter हैं, लेकिन node peer path इन्हें use नहीं करता; traffic plaintext है।<br><small>TLS primitives exist but are not wired into node connections.</small> |
| 14 | Guardian recovery।<br><small>Guardian recovery.</small> | ✅ Guardian recovery logic मौजूद।<br><small>Guardian recovery logic exists.</small> |
| 15 | RWA asset record।<br><small>RWA asset record.</small> | ⚠️ Asset registry/registration/transfer मौजूद; legal title या complete RWA platform नहीं।<br><small>Registry operations exist; not legal title or a full RWA platform.</small> |
| 16 | बड़े message का chunking।<br><small>Chunk large messages.</small> | ✅ Bounded chunks और reassembly checks मौजूद।<br><small>Bounded chunking and reassembly checks exist.</small> |
| 17 | एक chain file पर एक writer।<br><small>One writer per chain file.</small> | ✅ Permanent sidecar writer lock मौजूद।<br><small>Permanent sidecar writer lock exists.</small> |
| 18 | बहुत पीछे वाला node sync करे।<br><small>A far-behind node can sync.</small> | ⚠️ Missing-block sync मौजूद; बड़े lag की performance/scale प्रमाणित नहीं।<br><small>Missing-block sync exists; large-lag scale is unproven.</small> |
| 19 | Wallet से POST /tx।<br><small>Wallet transaction submission via POST /tx.</small> | ❌ API read-only/GET-only है; POST /tx route नहीं।<br><small>The API is read-only; no POST /tx route.</small> |
| 20 | Explorer live API से data दिखाए।<br><small>Explorer reads live API data.</small> | ❌ Explorer loaded JSON file verify करता है; live API से जुड़ा नहीं।<br><small>Explorer verifies a loaded file, not a live API.</small> |

## B. जो कभी नहीं होना चाहिए / Must never happen

| # | नियम / Rule | कोड में पुष्टि / Code-checked status |
|---:|---|---|
| 1 | “असली पैसा” या रुपये का दावा नहीं।<br><small>No real-money or rupee claim.</small> | ✅ Project docs इसे local prototype/token model कहते हैं; यह product-claim rule है, consensus invariant नहीं।<br><small>Docs identify a prototype; this is a claims rule.</small> |
| 2 | Save fail पर node disk/state mismatch के साथ आगे न चले।<br><small>No silent continuation after save failure.</small> | ✅ Producer save failure पर exit करता है; persistence failure लौटाती है।<br><small>Producer exits on save failure.</small> |
| 3 | Unsigned transaction स्वीकार न हो।<br><small>Never accept an unsigned transaction.</small> | ✅ Signature और public-key/address binding validate होते हैं।<br><small>Signature and key/address binding are validated.</small> |
| 4 | Public API बिना TLS खुला न हो।<br><small>No public API without TLS.</small> | ⚠️ API loopback-only है; peer TCP plaintext है। API को public expose न करें।<br><small>API is loopback-only; peer traffic is plaintext.</small> |
| 5 | Unbounded connections/resource capture न हो (F4)।<br><small>No unbounded connections/resource capture.</small> | ⚠️ API में connection cap है; peer-network F4 बाकी है।<br><small>API has a cap; peer-network F4 is incomplete.</small> |
| 6 | पुराने chain data को protocol/genesis जाँच बिना न पढ़ें।<br><small>Never load old chain data without protocol/genesis checks.</small> | ✅ Loader protocol, chain ID और genesis commitment/pin जाँचता है; unsupported version reject होती है।<br><small>Loader checks protocol, chain ID, and genesis pin.</small> |
| 7 | Existing file पर fresh genesis overwrite न हो।<br><small>Never overwrite an existing file with fresh genesis.</small> | ✅ Exclusive creation existing file को writer lock के अंदर refuse करती है।<br><small>Exclusive creation refuses existing files under lock.</small> |
| 8 | Operator से limits न छुपें; docs में लिखें।<br><small>Document operational limits.</small> | ⚠️ ज्ञात सीमाएँ यहाँ दर्ज हैं; future limits को भी docs में जोड़ना होगा।<br><small>Known limits are recorded; future limits must also be documented.</small> |

## C. बचा हुआ काम — क्रम में / Remaining work, in order

### Testnet चालू करने के लिए / To start a testnet
1. --peer <address>:<port> — अभी --peer <port> loopback-only है।<br><small>Add remote peer address support.</small>
2. दो अलग मशीनों पर connection और sync test।<br><small>Test cross-machine connection and sync.</small>
3. F7 — producer failover/continuity।<br><small>Producer failover/continuity.</small>
4. F4 — connection-cost/slow-peer protection।<br><small>Connection-cost and slow-peer protection.</small>
5. TLS को node peer path से जोड़ना और handshake test।<br><small>Integrate TLS into the peer path and test handshake.</small>

### उपयोगी बनाने के लिए / To make it useful
6. Signed transaction submission API: POST /tx।<br><small>Signed transaction submission API.</small>
7. Explorer को live/read-only API से जोड़ना।<br><small>Connect Explorer to the live read-only API.</small>
8. हर network का अलग configurable chain_id।<br><small>Configurable chain ID per network.</small>
9. F9 — recovery में अटके funds का समाधान।<br><small>Resolve funds stuck in recovery.</small>
10. F12 — हर block पर पूरी chain लिखने की लागत/व्यवहार।<br><small>Address full-chain write cost/behavior per block.</small>

### बाद में / Later
Consensus, Digital ID, marketplace. ये इस checklist में implemented नहीं माने गए।
Consensus, Digital ID, and marketplace are not claimed as implemented here.

## निरीक्षण सीमा / Review boundary

“मौजूद” का अर्थ code path मौजूद है; इससे cross-machine validation, production security, consensus/finality, economic safety या legal validity सिद्ध नहीं होती।
“Implemented” means a code path exists; it does not prove cross-machine validation, production security, consensus/finality, economic safety, or legal validity.
