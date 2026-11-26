# TAZVERA Chain local prototype: स्थिति और सीमाएँ

## यह क्या है?

यह repository का local prototype है, सार्वजनिक testnet या mainnet नहीं। यह मौजूदा Shardeum TZV token से अलग है। इसमें असली पैसे या असली wallet/private keys इस्तेमाल न करें। इस prototype की कोई घोषित कीमत नहीं है; इसकी कोई बाजार कीमत या redeem value नहीं है।

## नोड चलाने की शर्तें

- C++26 समर्थित compiler, CMake 3.20+ और OpenSSL 3.x development files चाहिए।
- Windows पर MSYS2 UCRT64 terminal और उसके matching compiler/OpenSSL packages इस्तेमाल करें।
- हर existing chain खोलते समय trusted genesis commitment देना ज़रूरी है।
- networking अभी केवल उसी मशीन के 127.0.0.1 loopback पर है। दो अलग कंप्यूटरों को जोड़ना समर्थित नहीं।
- चलाने के पूरे कदम: docs/RUN-A-NODE.md देखें।

## अभी क्या काम करता है

- स्थानीय chain file बनाना और trusted genesis pin के साथ load करना।
- Ed25519 transaction signing/verification और prototype transfer fee/nonce/balance checks।
- स्थानीय block बनाना, chain file में save/load करना और chain state/hash जाँचना।
- एक ही मशीन पर localhost peer से blocks sync करना।
- Optional read-only HTTP API; यह default में बंद है और loopback पर ही सुनता है।

## अभी क्या नहीं है / किस पर भरोसा न करें

- यह public testnet या production network नहीं है; कोई public endpoint या public peer network नहीं।
- अलग-अलग कंप्यूटरों के बीच P2P connectivity नहीं।
- Peer authentication नहीं; local prototype को hostile network के लिए सुरक्षित न मानें।
- Local demo sync को production network की सुरक्षा या स्थिरता की guarantee न मानें।
- Wallet, smart contracts, staking या production validator economics नहीं।
- HTTP API में transaction submit/write endpoint नहीं है।
- Prototype transfer fee 1000 utzv है और local model में burn होती है; यह अंतिम या approved fee नहीं।
- Local build/tests पास होना security audit या real-funds readiness का प्रमाण नहीं।

## गड़बड़ी होने पर

1. Node बंद करें और exact error message नोट करें।
2. Build बदलने के बाद पुराना process बंद करके binary फिर build करें: cmake --build build --parallel.
3. Chain load न हो तो सही trusted genesis pin दें। Pin को chain file से अनुमान लगाकर न बनाएं।
4. Peer connect न हो तो दोनों commands में एक ही port, एक ही genesis pin और सही chain file जाँचें। Server 127.0.0.1 पर चल रहा हो; अलग computer से कोशिश न करें।
5. Port busy हो तो server बंद करें या कोई खाली local port चुनें और दोनों तरफ वही port रखें।
6. Transfer fail हो तो sender balance में amount के साथ 1000 utzv fee और सही nonce की जगह जाँचें।
7. Logs/error से कारण स्पष्ट न हो तो reproduction steps और error output issue में दें। Secret key file या private seed कभी attach न करें।

**सुरक्षा नोट:** यह software testing के लिए है। इससे real funds भेजना, मूल्य का दावा करना, या इसे production/testnet के रूप में प्रचारित करना उचित नहीं है।
