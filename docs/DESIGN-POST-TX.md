# DESIGN: POST /tx — signed transaction submission
<small>Design only. No implementation in this change.</small>

## उद्देश्य और दायरा

यह दस्तावेज़ अगले API काम का प्रस्ताव है। मौजूदा HTTP API read-only है और GET-only है; `POST /tx` अभी उपलब्ध endpoint नहीं है। यह डिज़ाइन केवल signed transaction स्वीकार करने के लिए है। यह consensus, block production, TLS या remote API access शुरू करने की अनुमति नहीं देता।

## Endpoint और request format

- Method/path: `POST /tx`
- Content-Type: `application/json`
- Body shape: `{"tx":"<hex>" }`
- `tx` में एक पूरी signed transaction की canonical wire serialization के bytes का lowercase hexadecimal रूप होगा—signature सहित।
- Serialization/version, byte order, field lengths और exact decode rules को implementation से पहले protocol document में स्थिर करना होगा। JSON fields को अलग-अलग transaction fields की तरह स्वीकार न करें; signature उसी exact serialized message पर verify होनी चाहिए।
- Hex decode के बाद malformed, non-canonical या trailing bytes वाली request अस्वीकार करें। Request में private key/seed कभी न भेजें।

Successful first admission: `202 Accepted`, with a transaction ID and status such as `{"txid":"<64-hex>","status":"queued"}`. This means queued, **not confirmed**. Invalid request: appropriate 4xx with a short stable error code; do not expose secrets or internal stack traces.

## Server validation order

किसी transaction को mempool में रखने से पहले:

1. Request method, Content-Type, JSON shape, hex syntax और decoded byte length जाँचें।
2. Canonical transaction decode करें; unknown version/kind और non-canonical encoding अस्वीकार करें।
3. Protocol version और `chain_id` जाँचें; दूसरी chain की transaction स्वीकार न करें।
4. Sender address और public key का मेल जाँचें; signature को exact signed bytes पर verify करें।
5. Transaction shape, destination, amount overflow और तय fee schedule जाँचें। Fee इस chain के वर्तमान नियम से मेल खाए; client अपनी fee तय न कर सके।
6. Sender का current nonce और उपलब्ध balance जाँचें। Balance में amount + fee दोनों के लिए पर्याप्त राशि होनी चाहिए। Nonce सिर्फ़ current chain state के आधार पर नहीं, mempool में sender की पहले से queued transactions के क्रम के साथ भी जाँचना होगा।
7. Expiry लागू हो तो उसे जाँचें। फिर ही transaction को bounded mempool में डालें।

ये checks admission के समय प्रारंभिक हैं; block बनाते/स्वीकारते समय सभी consensus-validity checks फिर से लागू होंगे। Admission validation block confirmation नहीं है।

## Replay और duplicate handling

- Transaction ID signed canonical bytes (signature सहित) के SHA-256 से निकले।
- Mempool में वही txid दोबारा आए तो उसे दूसरी बार queue न करें। मौजूदा txid और `already_known` status लौटाएँ; duplicate को नया fee/nonce खर्च करने वाली अलग transaction न मानें।
- जो transaction पहले ही block में शामिल हो चुकी है, उसका nonce chain state में आगे बढ़ चुका होगा; दोबारा भेजने पर `bad_nonce`/already-included response दें और उसे apply न करें।
- एक ही sender के लिए एक nonce पर अलग-अलग signed transactions में से केवल एक को सक्रिय queue entry मिले। Conflict को deterministic तरीके से reject/replace करें; चुपचाप दोनों न रखें।
- Reorg/fork support आने पर included/queued index को canonical chain के अनुसार फिर से बनाना होगा। इस prototype में fork/reorg पर mempool behavior को implementation से पहले स्पष्ट करना आवश्यक है।

## Resource limits (प्रस्तावित defaults)

ये नए POST endpoint के प्रस्तावित limits हैं, मौजूदा GET API के दावे नहीं:

- Maximum HTTP request body: **64 KiB** (JSON सहित); इससे बड़ी body पर `413 Payload Too Large` और connection close।
- Maximum decoded signed transaction: **32 KiB**; इससे बड़ी transaction reject।
- Rate limit: प्रति loopback client **10 requests/second**, burst **20**; exceed होने पर `429 Too Many Requests`। Invalid requests भी limit में गिने जाएँ।
- Concurrent connections: मौजूदा API की सीमा **8** ही रखें; POST के लिए अलग pool बनाकर कुल सीमा न बढ़ाएँ।
- Request timeout: मौजूदा **10 seconds** ही; slow body upload timeout पर connection close।
- Mempool: अधिकतम **10,000 transactions** और **16 MiB** serialized bytes; full होने पर स्पष्ट `503`/busy response, silently drop न करें।
- Limits configuration code/API implementation से पहले tests में pinned हों। Loopback-only deployment rate limit को public-network protection न समझें।

## Mempool बनाम सीधे block बनाना

**POST handler से सीधे block बनाना अनुशंसित नहीं है।** इससे हर request block production trigger करेगी, competing requests के क्रम पर परिणाम निर्भर होगा, batching/nonce ordering कठिन होगी, और API request सीधे chain mutation कर देगी।

अनुशंसित flow: validate → bounded mempool → existing block producer/explicit block workflow transaction चुनता है → block validation/commit → node sync करता है। लाभ: batching, deduplication, sender nonce ordering और API से block production अलग। नुकसान: queued transaction की पुष्टि की गारंटी नहीं, mempool eviction/restart behavior चाहिए, और inclusion में देरी हो सकती है।

वर्तमान node में autonomous block producer/mempool submission API नहीं है। इस design को लागू करने से पहले producer integration, queue persistence policy, expiry/eviction और shutdown behavior अलग से तय हों। POST endpoint को खुद block बनाने का shortcut न दें।

## CORS और Host सुरक्षा

मौजूदा API के CORS और Host validation नियम ज्यों के त्यों रहें:

- Host केवल configured port पर `localhost:<port>` या `127.0.0.1:<port>` के accepted forms में हो; duplicate/invalid Host reject हो।
- Origin केवल valid exact origin हो और configured allowlist में हो; wildcard `*` स्वीकार न करें। Missing Origin वाले non-browser clients के मौजूदा व्यवहार को न बदलें।
- POST जोड़ने के नाम पर permissive CORS, wildcard origin या Host-check bypass न जोड़ें। CORS authentication नहीं है।

## Network exposure / TLS

**यह endpoint सिर्फ़ loopback पर रहेगा जब तक TLS implement नहीं होता।** Bind address को `127.0.0.1` तक सीमित रखें; `0.0.0.0`, LAN या public interface पर POST enable न करें। मौजूदा API के Host/CORS checks TLS का विकल्प नहीं हैं। TLS आने के बाद भी remote exposure के लिए अलग security review और explicit configuration चाहिए।

## Tests required before implementation is accepted

1. Valid signed transaction accepted and queued; response contains stable txid/status.
2. Invalid JSON, missing/extra fields, malformed hex, odd-length hex, non-canonical serialization, trailing bytes.
3. Body exactly at limit accepted; one byte over returns 413; decoded tx limit enforced.
4. Unsupported protocol version, wrong chain_id, unknown kind, invalid shape and wrong fee rejected.
5. Valid signature accepted; corrupted signature, wrong sender/public-key pair rejected.
6. Exact duplicate while queued returns already-known and queue size does not grow.
7. Already-included transaction is not queued/applied again; nonce conflict from same sender handled deterministically.
8. Correct nonce accepted; stale/skipped nonce rejected; balance checks include amount + fee; fee burn occurs only when a block commits.
9. Queue full by count/bytes returns explicit busy response; eviction/expiry and restart behavior match the documented policy.
10. Rate limit burst/refill, invalid-request counting, and 429 response.
11. Connection cap, timeout, oversized slow upload, and connection cleanup.
12. Host spoofing, duplicate Host, disallowed Origin, wildcard Origin, and allowed exact Origin behavior remain unchanged.
13. Bind test proves POST listener only accepts loopback; attempts on non-loopback bind are rejected.
14. Accepted/queued transaction does not change balance, nonce, block height or state root until a valid block is committed.
15. Node syncs the committed block and independently validates the transaction; API response is never described as confirmation.
16. Regression: existing GET endpoints and current GET-only behavior remain unchanged until this endpoint is intentionally implemented.

## Open decisions to resolve before coding

- Publish the exact canonical transaction wire format and a protocol/version identifier.
- Decide whether mempool survives restart and how it reconciles with accepted blocks/forks.
- Pin stable error codes and duplicate/included response semantics.
- Confirm the 64 KiB / 32 KiB / 10 req/s / burst 20 / 10,000 tx / 16 MiB defaults against expected workload.
- Specify how block producer selects transactions and handles sender nonce order.
