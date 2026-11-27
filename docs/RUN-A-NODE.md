# अपनी मशीन पर TAZVERA node चलाना

यह guide node को repository source से CMake द्वारा build करने के लिए है; अभी कोई downloadable release जारी नहीं हुई है।
वर्तमान CLI में `--produce-every` उपलब्ध है; यह `--serve` के साथ हर N seconds में block बनाने के लिए है। `--tls` flag/transport अभी implement नहीं है।
ध्यान दें: `--every <seconds>` केवल `--peer` reconnect interval है, block-production interval नहीं।
यह local development prototype है, public testnet या payment service नहीं। यहाँ के balances/transactions test data हैं, आर्थिक मूल्य नहीं। Private keys/seed साझा न करें। इस guide में दोनों नोड **एक ही कंप्यूटर पर** चलते हैं।

## 1. Source से build करें

अभी कोई release binary डाउनलोड करने के लिए उपलब्ध नहीं है। Repository का source clone करके CMake से binary बनाएं। दो मशीनों के test में दोनों पर compatible binaries उसी commit से बननी चाहिए।

Linux:
```sh
git clone https://github.com/tazvera/tazvera-chain.git
cd tazvera-chain
git checkout main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows 10/11 पर MSYS2 UCRT64 terminal खोलकर project की Windows build instructions के अनुसार dependencies install करें, फिर CMake configure/build/test चलाएँ। Compiler को C++26 support करना होगा। Build/test fail हो तो आगे न बढ़ें।

## 2. Test keys और genesis बनाएं

Project root से दो test keys बनाएं:
```sh
./build/taznode --new-key node-a.key
./build/taznode --new-key node-b.key
```
Windows पर executable `build/taznode.exe` है। छपे हुए दोनों public addresses नोट करें। Private key files साझा न करें।

एक नई chain बनाएं; addresses को वास्तविक test addresses से बदलें:
```sh
./build/taznode --chain local-chain.json --init-genesis ADDRESS_A:100000000 --init-genesis ADDRESS_B:100000000
```
Genesis commitment/pin को ज्यों का त्यों सुरक्षित लिख लें। अलग addresses, balances या options से pin अलग होगा। Existing chain file overwrite नहीं होती; जरूरत हो तो नई filename लें।

## 3. Pin verify करें

```sh
./build/taznode --chain local-chain.json --expect-genesis <pin>
```
`<pin>` को पूरे genesis commitment से बदलें। सही pin पर chain load होनी चाहिए; गलत pin पर आगे न बढ़ें।

## 4. Block producer चलाएं

`--produce-every` उपलब्ध है। Producer को `--serve` और trusted genesis pin के साथ चलाएं। Producer-enabled chain पर `--key-file` में उसी chain के configured producer की private key होनी चाहिए।

उदाहरण — हर 5 सेकंड में block बनाने की कोशिश:
```sh
taznode --chain a.json --expect-genesis <pin> --key-file a.key --serve 7001 --produce-every 5
```
`<pin>` को वास्तविक trusted genesis pin से बदलें। `a.key` उसी producer की key होनी चाहिए जिसे chain में producer के रूप में तय किया गया है। यह command लगातार चलती रहेगी; रोकने के लिए Ctrl+C दबाएं।

**सीमाएँ / जरूरी व्यवहार:**
- Block हर N सेकंड में schedule होता है, लेकिन समय थोड़ा खिसक सकता है। Node single-threaded है; peer का लंबा काम अगला tick देर से करा सकता है।
- ये blocks खाली होंगे। `taznode` में mempool नहीं है, इसलिए pending transactions अपने-आप इन blocks में शामिल नहीं होतीं।
- जिस chain में producers तय हैं, block बनाने के लिए उसी configured producer की key चाहिए। दूसरे producer की key से tick refuse/skip हो सकता है।
- यदि tip का timestamp local clock से 120 सेकंड या अधिक आगे है, node warning देता है और उस tick को छोड़ देता है; clock catch up होने तक इंतजार करता है।
- Block chain file में save न हो सके तो node exit code 3 से बंद होगा।
- `--produce-every` को `--serve` के बिना देना usage error है (exit code 2)।
- एक ही chain पर दो producer चलाना fork race पैदा कर सकता है। इस option का intended setup एक producer और followers हैं; यह consensus/finality नहीं है।

## 5. दो नोड एक ही मशीन पर चलाएं

दो terminals खोलें। Peer node के लिए chain file की अलग copy बनाएं:

```sh
cp local-chain.json peer-chain.json
```

Windows Command Prompt में `copy local-chain.json peer-chain.json` चलाएं।

**Terminal 1 — server/producer:** ऊपर दिए गए producer command में `a.json` को `local-chain.json` और port को `7001` रखें।

**Terminal 2 — peer:**
```sh
./build/taznode --chain peer-chain.json --expect-genesis <pin> --peer 7001
```

Windows पर executable के लिए `build/taznode.exe` लिखें। Server और peer दोनों terminals खुली रखें; logs में connection/sync देखें। Peer को producer की अलग chain-file copy दें; एक ही file को दो processes से लिखवाएं नहीं।

## 6. अलग मशीनों के बारे में जरूरी बात

`--listen-on <IPv4-address>` मौजूद है; इसे `--serve <port>` या `--listen <port>` के साथ दें। Default listener address `127.0.0.1` है। `--peer <port>` अभी केवल `127.0.0.1:<port>` पर connect करता है; `--peer <address>:<port>` वाला remote-address विकल्प अभी उपलब्ध नहीं है। इसलिए दो नोड एक ही मशीन पर चलाएं।

## 7. आम गलतियाँ

- **Genesis pin mismatch:** दोनों nodes में वही पूरा pin दें; genesis inputs भी समान हों।
- **Port busy / bind failed:** पुराना node रोकें या खाली port चुनें; दोनों commands में वही port रखें।
- **Unknown flag:** सही commit से binary rebuild करें और `--help` देखें।
- **Height नहीं बढ़ती:** producer की logs देखें; configured producer key, clock warning और chain-file write errors जाँचें।
- **Chain load/parse error:** node रोकें, logs सुरक्षित करें; chain file हाथ से edit न करें। केवल disposable test data के लिए नई filename से genesis बनाएं।

Test खत्म होने पर node terminals में **Ctrl+C** दबाकर रोकें। Chain file को हाथ से न बदलें और private keys साझा न करें।
