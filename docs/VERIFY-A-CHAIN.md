# चेन की जाँच कैसे करें
# How to verify a chain

यह मार्गदर्शिका मिली हुई chain file को दर्ज किए गए TAZVERA local prototype से मिलाने के लिए है। यह कानूनी स्वामित्व या public network का प्रमाण नहीं है।
This guide compares a received chain file with the recorded TAZVERA local prototype. It is not proof of legal ownership or a public network.

## 1. Genesis pin: भरोसेमंद स्रोत बाहर से लें
## 1. Genesis pin: obtain it independently

**Pin chain file से न लें।** उसे स्वतंत्र भरोसेमंद स्रोत से लें—जैसे signed release/official published record, या पहले से अलग माध्यम से पुष्टि किया रिकॉर्ड। उसी file में दिया pin लेकर उसी file को असली कहना जाँच नहीं है; कोई भी अपनी chain और उसका pin साथ बना सकता है।
**Do not take the pin from the chain file.** Obtain it independently from a trusted source, such as a signed release/official published record or a record confirmed earlier through a separate channel. A pin found only in the same file is not verification.

इस दर्ज local run का pin / Pin for this recorded run:

```text
5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63
```

1. बाहरी रिकॉर्ड से पूरा pin कॉपी करें; अक्षर न बदलें। / Copy the complete pin from the external record without altering it.
2. प्राप्त file की copy खोलते समय वही pin दें:
   Open a copy of the received file with the trusted pin:
   ```sh
   taznode --chain received-chain.json --expect-genesis <trusted-pin>
   ```
3. Placeholder को असली pin से बदलें। Mismatch/load error आए तो file स्वीकार न करें। सफल load केवल genesis commitment का मेल बताता है; बाकी जाँचें भी करें।
   Replace the placeholder with the real pin. Reject the file on mismatch/load error. A successful load only confirms the genesis commitment; continue checking.

## 2. State root: हर block के बाद की स्थिति
## 2. State root: state after each block

`state_root` उस block की transactions लागू करने के बाद बनी ledger/state का commitment है—जैसे balances और nonces। Genesis से blocks क्रम में लागू करें, हर height पर state दोबारा निकालें, और निकला root उसी block के `state_root` से मिलाएँ। किसी भी mismatch पर जाँच रोकें।
`state_root` commits to the ledger/state after applying a block, such as balances and nonces. Apply blocks in order from genesis, recompute state at each height, and compare the result with that block's `state_root`. Stop on any mismatch.

API से मिला root उसी node का दावा है, स्वतंत्र पुनर्गणना नहीं। इस prototype के लिए documented standalone verifier की पुष्टि नहीं है; API output को recomputation न मानें।
A root returned by the API is that node's claim, not an independent recomputation. A documented standalone verifier has not been confirmed for this prototype; do not treat API output as recomputation.

## 3. Block hash और chain links
## 3. Block hash and chain links

Block hash protocol-defined serialized block content का digest है, raw JSON text का मनमाना hash नहीं। Implementation तय करता है कि कौन-से fields और encoding शामिल हैं।
A block hash is a digest of protocol-defined serialized block content, not an arbitrary hash of raw JSON. The implementation defines included fields and encoding.

- हर block का hash और previous-block hash/reference देखें; हर reference ठीक पिछले block के hash से मेल खाना चाहिए।
  Inspect each block hash and previous-block reference; each reference must match the immediately preceding block's hash.
- Block content बदलने पर उसका hash बदलना चाहिए; अगला block पुराने hash को refer करेगा, इसलिए link टूटेगा और उसके बाद की chain-link जाँच fail होगी।
  Changing a block's content should change its hash; the next block still refers to the old hash, breaking the link and subsequent chain-link checks.
- Stored/API hash पढ़ना स्वतंत्र verification नहीं है। Raw JSON पर generic SHA-256 न चलाएँ जब तक canonical serialization और implementation का algorithm न अपनाया हो।
  Reading a stored/API hash is not independent verification. Do not hash raw JSON with generic SHA-256 unless using the implementation's canonical serialization and algorithm.

## 4. एक block में transfer गिनें
## 4. Count transfers in a block

1. `GET /block/131` लें और transaction list की entries गिनें। इस दर्ज example में block 131 में **एक transfer** है।
   Fetch `GET /block/131` and count transaction entries. The recorded example has **one transfer** in block 131.
2. Entry में sender, recipient, amount, fee और nonce जैसे उपलब्ध fields देखें। Sender k1, recipient k2 और amount 1000 है।
   Inspect available sender, recipient, amount, fee, and nonce fields. Sender is k1, recipient is k2, amount is 1000.
3. Transaction entry को post-block balances से मिलाएँ; entry मौजूद होना अपने-आप सही application साबित नहीं करता।
   Compare the transaction entry with post-block balances; its presence alone does not prove correct application.

## 5. Read-only API जाँच
## 5. Read-only API checks

API default में बंद है और loopback पर सुनती है। `<api-base>` को node की वास्तविक API base URL से बदलें, जैसे `http://127.0.0.1:<port>`; port configuration से लें, अनुमान न लगाएँ।
The API is disabled by default and listens on loopback. Replace `<api-base>` with the node's actual API base URL, such as `http://127.0.0.1:<port>`; use the configured port, do not guess.

```sh
curl <api-base>/status
curl <api-base>/block/131
curl <api-base>/balance/c68b2d0cb47a57e30834fc235b8422bfdc09d728
curl <api-base>/balance/f46c08954f95a092e225d33d9b3ee379c05c14b7
```

- `GET /status`: chain identity, protocol और height जैसे उपलब्ध status fields देखें। इस example में chain_id `tazvera-local-prototype-v1`, protocol `3`, producer height कम-से-कम 131। Actual response field names देखें।
  `GET /status`: inspect available identity, protocol, and height fields. Here chain_id is `tazvera-local-prototype-v1`, protocol `3`, producer height at least 131. Use actual response field names.
- `GET /block/131`: height, hash, previous hash, state root और transaction list में जो fields वास्तव में लौटें उन्हें जाँचें; एक transaction अपेक्षित है।
  `GET /block/131`: check returned height, hash, previous hash, state root, and transaction list; one transaction is expected.
- `GET /balance/<address>`: दोनों खातों के balance/nonce देखें। दर्ज run के बाद k1 `998000`, nonce `1`; k2 `1000`, nonce `0` था।
  `GET /balance/<address>`: inspect both accounts' balance/nonce. After the recorded run, k1 was `998000`, nonce `1`; k2 was `1000`, nonce `0`.

Mismatch हो तो दर्ज करें और उस file/node पर भरोसा न करें। एक ही node के कई API responses स्वतंत्र स्रोत नहीं हैं।
Record any mismatch and do not trust that file/node. Multiple API responses from one node are not independent sources.

## 6. छोटा दर्ज उदाहरण
## 6. Short recorded example

- Genesis pin: `5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63`
- Block 131: k1 से k2 को 1000 का एक transfer
- बाद में k1 balance `998000`, nonce `1`; k2 balance `1000`, nonce `0`
- k1 से कुल `2000` घटे: `1000` transfer + `1000` fee

ये 27 नवंबर 2026 के दर्ज local-run आँकड़े हैं, भविष्य के run की गारंटी नहीं।
These are recorded local-run figures for 27 November 2026, not a guarantee for future runs.

## 7. यह जाँच क्या साबित नहीं करती
## 7. What these checks do not prove

- **कानूनी स्वामित्व:** address/balance यह नहीं बताते कि संपत्ति का कानूनी मालिक कौन है; उसके लिए कानूनी दस्तावेज़ और लागू कानून की जाँच चाहिए।
  **Legal ownership:** an address/balance does not identify the legal owner; legal documents and applicable law must be reviewed.
- **Consensus/सही संचालन:** ये जाँचें साबित नहीं करतीं कि chain सही चली, सभी nodes सहमत थे, या fork नहीं था। Prototype में production consensus/finality नहीं है; hashes और state roots consensus नहीं बनाते।
  **Consensus/correct operation:** these checks do not prove correct operation, node agreement, or absence of forks. The prototype has no production consensus/finality; hashes and state roots do not create consensus.
- **पूर्ण सुरक्षा:** pin match, API response या hash links software, operator, key custody, network security या आर्थिक दावों की पूरी गारंटी नहीं हैं।
  **Complete security:** a matching pin, API response, or hash links do not fully guarantee software, operator, key custody, network security, or economic claims.

**सुरक्षा:** यह local prototype है; public network पर न चलाएँ और private key/seed कभी साझा न करें।
**Safety:** this is a local prototype; do not run it on a public network and never share a private key/seed.
