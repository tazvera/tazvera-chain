# TAZVERA testnet: पहले दिन की checklist

यह development test है, public testnet launch नहीं। असली funds/private keys इस्तेमाल न करें। **आज की सीमा:** `--listen-on` उपलब्ध है, लेकिन `--peer` अभी केवल `127.0.0.1` पर connect करता है। B से A के LAN address पर जुड़ने का `--peer <address>:<port>` विकल्प अभी बाकी है; Claude बना रहा है। इसलिए दो-मशीन sync वाला चरण तभी करें जब वह विकल्प merge होकर test हो जाए। तब तक एक ही मशीन पर दो नोड चलाकर localhost sync जाँचें।

## 1. दोनों मशीनों पर binary build करें

- A और B पर repository का एक ही commit checkout करें; commit SHA लिख लें।
- दोनों पर dependencies, configure, build और tests चलाएँ। दोनों builds सफल होने चाहिए।
- दोनों binaries उसी commit की हों। पुराना binary चल रहा हो तो उसे बंद करें और नया build करें।

## 2. Keys बनाएं, chain बनाएं, pin मिलाएं

- A और B पर test-only node keys बनाएं। Public addresses नोट करें; private key/seed किसी को न भेजें।
- Genesis में इस्तेमाल होने वाले public addresses और balances का क्रम तय करें। A और B पर **ठीक वही addresses, वही balances, वही क्रम और वही options** इस्तेमाल करें।
- दोनों मशीनों पर अलग, नई chain files बनाएं: A पर `a.json`, B पर `b.json`। एक ही genesis command/options चलाएं।
- दोनों outputs से पूरा genesis commitment/pin लिखें। **Pin byte-for-byte समान होना चाहिए।** अलग हो तो आगे न बढ़ें; inputs जाँचकर नई disposable chain files बनाएं।
- दोनों nodes को `--expect-genesis <pin>` में यही pin दें।

## 3. A को server की तरह चलाएं और LAN address जाँचें

- A पर `ipconfig` (Windows) या `ip addr` (Linux) से connected LAN/Wi-Fi interface का IPv4 address निकालें। `127.0.0.1`, VPN या virtual adapter का address नहीं।
- दोनों मशीनें भरोसेमंद उसी LAN/Wi-Fi पर हों; guest Wi-Fi/client isolation न हो।
- Remote-peer address support उपलब्ध होने पर A पर चलाएं:

```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001 --listen-on <A का LAN पता>
```

- A की terminal खुली रखें और log में सफल bind/listen देखें। LAN address और port 7001 सही होने की पुष्टि करें।

## 4. B से जुड़ें और sync/height देखें

Remote `--peer` address विकल्प merge होकर उपलब्ध और test होने के बाद B पर:

```sh
taznode --chain b.json --expect-genesis <pin> --peer <A का LAN पता>:7001
```

B के logs में connection/sync देखें। Height तभी बढ़ेगी जब A के पास B से आगे के blocks हों; A के पास नए blocks न हों तो height का न बढ़ना अपने-आप failure नहीं है। Genesis pin दोनों तरफ समान होना चाहिए।

**अभी का fallback:** B से A के LAN address पर connect करने का `--peer` address विकल्प उपलब्ध नहीं है। तब तक दोनों नोड एक ही computer पर चलाएं: A/server `--serve 7001 --listen-on 127.0.0.1`, B/peer `--peer 7001`; chain files अलग copies हों।

## 5. Connection न बने तो एक-एक करके जाँचें

1. **A चल रहा है?** Server process और bind/listen log देखें।
2. **पता सही है?** A का connected LAN IPv4 लें; B command में वही address और port 7001 हो।
3. **Firewall?** A पर भरोसेमंद Private LAN के लिए केवल TCP 7001 inbound allow करें। Firewall पूरी तरह बंद न करें; test के बाद temporary rule हटाएँ।
4. **Port busy?** Bind error/address already in use आए तो पुराने node को रोकें या खाली port चुनें; A और B दोनों में वही नया port रखें।
5. **पुराना binary?** A/B का commit SHA और build path मिलाएं; दोनों पर उसी commit से rebuild करें।
6. **Network isolation?** दोनों एक ही trusted Wi-Fi/LAN पर हों; guest network, VPN, router client/AP isolation और third-party firewall जाँचें।
7. **Pin mismatch?** दोनों genesis pins और genesis inputs मिलाएं। गलत pin से sync आगे न बढ़ाएं।
8. **Logs/height:** दोनों terminals के पूरे relevant logs और दोनों heights दर्ज करें। केवल ping success को TCP connection का प्रमाण न मानें।

## 6. Test का रिकॉर्ड रखें

- तारीख/समय और test किसने/किन मशीनों पर किया।
- दोनों machines के OS और node binary का commit SHA; build/test result।
- A/B के genesis pin (पूरा या सुरक्षित record में) और यह कि वे समान थे।
- A का LAN address (record को private रखें), इस्तेमाल किया port और A/B की exact commands।
- Connection सफल हुआ या नहीं; sync से पहले/बाद A और B की heights; relevant log lines।
- Firewall में अस्थायी बदलाव किए हों तो कौन-से, और बाद में हटाए गए या नहीं।
- Failure हो तो exact error, किस चरण में आया, और ऊपर की कौन-सी जाँच की गई।

## 7. Nodes सुरक्षित तरीके से रोकें

- दोनों node terminals में **Ctrl+C** दबाएं और process के रुकने का इंतज़ार करें।
- अगर process नहीं रुकता, पहले terminal/log देखें; force-kill को आखिरी उपाय रखें।
- **Chain file को कभी हाथ से edit, truncate या “ठीक” न करें।** गड़बड़ पर node रोकें, logs और chain file की copy सुरक्षित रखें। केवल disposable test data होने की पुष्टि के बाद नई filename से नई chain बनाएं।
- Private keys/seed साझा न करें। Test पूरा होने पर temporary firewall rule हटाएं और public internet पर कोई port forward न रखें।

यह test LAN connectivity/sync का सीमित check है; production readiness, consensus safety, TLS या public testnet readiness का प्रमाण नहीं।