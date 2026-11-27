# Testnet शुरू करने से पहले की चेकलिस्ट

यह चेकलिस्ट **testnet शुरू करने की अनुमति** तय करने के लिए है। किसी बिंदु को तभी टिक करें जब उसका प्रमाण दर्ज हो। केवल build सफल होना या एक मशीन पर चलना पर्याप्त नहीं है।

## 1. कोड और सुरक्षा

- [ ] `--listen-on <IPv4-address>` का व्यवहार जाँचा है। यह `--serve <port>` या `--listen <port>` के साथ listener address सेट करता है; default `127.0.0.1` है।
- [ ] `--produce-every` अभी बना नहीं है; इसे उपलब्ध feature या tested behavior न मानें। CLI का `--every <seconds>` केवल `--peer` reconnect interval है, block-production interval नहीं।
- [ ] **F7 खुला है:** producer availability/slot skipping लागू और प्रमाणित नहीं है। Producer रुकने पर chain रुक सकती है; इसे बंद issue न मानें।
- [ ] **F4 खुला है:** bounded frame/message और sync batch/time/byte limits मौजूद हैं, लेकिन connection/request rate limit अभी नहीं है। इसे complete न मानें; rate-limit/cooldown design और abuse tests लागू होने के बाद ही पूरा मानें।
- [ ] नए और पुराने network tests पूरे चलाए गए; कोई test हटाया, skip किया या कमजोर assertion के साथ बदला नहीं गया।
- [ ] Linux और Windows पर build तथा test परिणाम दर्ज हैं; sanitizer/fuzz परीक्षण जहाँ लागू हों वहाँ सफल हैं।
- [ ] `--listen-on` का help/usage और error behavior सही है। `--produce-every` अभी बना नहीं है; उसके लिए कोई उपलब्ध CLI behavior claim न करें।

## 2. दो अलग मशीनों का परीक्षण

- [ ] दो अलग कंप्यूटर, एक ही घर के Wi-Fi/LAN पर, एक-दूसरे के LAN address से connect कर सकते हैं।
- [ ] दोनों nodes के genesis commitment समान है; गलत genesis पर sync अस्वीकार होता है।
- [ ] दोनों nodes hello/authentication पूरा करते हैं और एक-दूसरे की अपेक्षित identity/chain जानकारी जाँचते हैं।
- [ ] एक node पर block बनने के बाद दूसरा node उसे sync करता है; दोनों की chain files और heights की जाँच की गई है।
- [ ] Disconnect/reconnect, धीमा peer, गलत message और sync interruption के परीक्षण किए गए हैं; node hang या chain corruption नहीं होती।
- [ ] यह LAN परीक्षण **बिना TLS** किया गया है और उसकी सीमा समझी गई है; इसे सार्वजनिक इंटरनेट पर सुरक्षित connection का प्रमाण नहीं माना गया है।

## 3. चलाने के निर्देश

- [ ] `docs/RUN-A-NODE.md` को ऐसे दूसरे व्यक्ति ने, लेखक की मौखिक मदद के बिना, शुरू से अंत तक follow किया और node सफलतापूर्वक चलाया।
- [ ] उस व्यक्ति ने दो nodes चलाकर sync और shutdown भी पूरा किया।
- [ ] निर्देशों में इस्तेमाल किए गए flags, ports, file names और अपेक्षित output वर्तमान code से मिलते हैं।
- [ ] Windows और Linux के निर्देश जहाँ समर्थित हैं वहाँ अलग-अलग सत्यापित हैं; जो platform सत्यापित नहीं है उसे साफ़ लिखा गया है।

## 4. Testnet की सीमाएँ — शुरू करने से पहले पढ़ें

- [ ] **केवल test data:** balances/transactions का कोई घोषित आर्थिक मूल्य नहीं है और कोई real-world payment path नहीं है; इन्हें भुगतान या funds न समझें।
- [ ] Local prototype transfer को वास्तविक भुगतान या wallet/payment service के रूप में प्रस्तुत नहीं किया गया है.
- [ ] **TLS नहीं:** transport encrypted नहीं है। इसे केवल नियंत्रित, भरोसेमंद LAN परीक्षण तक सीमित रखें; सार्वजनिक Wi-Fi या इंटरनेट पर न चलाएँ।
- [ ] **Consensus नहीं:** nodes के बीच production-grade consensus/finality नहीं है। समान chain दिखना या sync पूरा होना canonical chain/finality की गारंटी नहीं है।
- [ ] Testnet को production, सुरक्षित, विकेंद्रीकृत या आर्थिक रूप से उपयोग योग्य network के रूप में प्रस्तुत नहीं किया जा रहा है।

## 5. गड़बड़ी होने पर

- [ ] **Node रुक गया:** terminal का पूरा error/log सुरक्षित करें; process को सामान्य तरीके से रोकें और उसी configuration से दोबारा चलाकर जाँचें। बार-बार crash हो तो testnet रोकें और issue दर्ज करें।
- [ ] **Sync नहीं हुआ:** दोनों nodes का genesis commitment, listen address/port, LAN connectivity, firewall, peer command और logs जाँचें। अलग genesis या गलत port को ठीक किए बिना chain files न मिलाएँ।
- [ ] **Chain file खराब हुई:** node रोकें और खराब file की copy सुरक्षित रखें। केवल disposable test data होने की पुष्टि के बाद नई test chain बनाएँ; खराब file को चुपचाप overwrite/delete न करें। आवश्यक logs और file अलग रखें।
- [ ] **अनपेक्षित block/height या protocol error:** testnet रोकें, दोनों nodes के logs और chain-file copies सुरक्षित करें, और कारण समझे बिना इसे सामान्य व्यवहार न मानें।
- [ ] समस्या का कारण और समाधान दर्ज किए बिना checklist को पूरा घोषित न करें।

## 6. अंतिम अनुमति

- [ ] ऊपर के सभी लागू बिंदुओं का प्रमाण दर्ज है और कोई unresolved critical/high सुरक्षा समस्या नहीं है।
- [ ] Testnet शुरू करने की अनुमति जिम्मेदार reviewer ने स्पष्ट रूप से दी है।
