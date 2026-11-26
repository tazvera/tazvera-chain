# अपनी मशीन पर TAZVERA node चलाना

> यह केवल local prototype है। असली पैसा, असली wallet key या Shardeum TZV इस्तेमाल न करें। नोड केवल 127.0.0.1 पर सुनता है। दूसरे कंप्यूटर से जुड़ना अभी समर्थित नहीं है; नीचे दो नोड एक ही कंप्यूटर पर चलेंगे।

## 1. ज़रूरी चीज़ें

- CMake 3.20 या नया
- OpenSSL 3.x development package
- C++26 compiler: GCC 16+, Clang 22+, या Visual Studio 2022 17.14+ का MSVC
- Git

## 2. कोड लें और build करें

### Linux (Ubuntu/Debian)

    sudo apt update
    sudo apt install git cmake build-essential libssl-dev
    git clone https://github.com/tazvera/tazvera-chain.git
    cd tazvera-chain
    git checkout 31291d6
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

**क्या दिखना चाहिए:** CMake configure और build सफल हों; CTest के अंत में 100% tests passed दिखे। यदि compiler C++26 support नहीं करता, configure वहीं रुक जाएगा।

### Windows 10/11 (MSYS2 UCRT64)

1. MSYS2 install करें और **MSYS2 UCRT64** terminal खोलें (साधारण MSYS terminal नहीं)।
2. package update करें:

    pacman -Syu

अगर terminal बंद करने को कहे तो बंद करें, UCRT64 फिर खोलें और update पूरा करें।

    pacman -S --needed git cmake mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-openssl
    git clone https://github.com/tazvera/tazvera-chain.git
    cd tazvera-chain
    git checkout 31291d6
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

**क्या दिखना चाहिए:** compiler, OpenSSL और CMake मिलें और tests pass हों। MSYS2 का उपलब्ध GCC अगर C++26 के लिए बहुत पुराना हो तो build नहीं होगा; उस स्थिति में नया समर्थित compiler इस्तेमाल करें।

## 3. दो अलग key बनाएं

हर command project root से चलाएं। पहले node A और फिर node B की key बनाएं:

    ./build/taznode --new-key node-a.key
    ./build/taznode --new-key node-b.key

Windows पर executable का नाम build/taznode.exe है:

    build/taznode.exe --new-key node-a.key
    build/taznode.exe --new-key node-b.key

**क्या दिखना चाहिए:** हर बार एक अलग 40-hex address छपे और key file बने। ये prototype keys हैं; इन्हें असली funds के लिए न रखें, साझा न करें।

## 4. नई local chain बनाएं

दोनों genesis accounts में मनमाना test balance दें। ADDRESS_A और ADDRESS_B को ऊपर छपे पूरे addresses से बदलें:

    ./build/taznode --chain local-chain.json --init-genesis ADDRESS_A:100000000 --init-genesis ADDRESS_B:100000000

Windows में ./build/taznode की जगह build/taznode.exe लिखें।

**क्या दिखना चाहिए:** chain file बने और node एक 64-hex **genesis commitment** छापे। उसे अलग सुरक्षित text file में copy करें; इसे chain file से खुद निकालकर भरोसे का pin न बनाएं। गलत address/amount या पहले से मौजूद chain file पर command fail होनी चाहिए। पुरानी file overwrite नहीं होती।

## 5. सही pin से chain खोलें

Terminal 1 में:

    ./build/taznode --chain local-chain.json --expect-genesis GENESIS_HASH

GENESIS_HASH को step 4 के पूरे commitment से बदलें।

**क्या दिखना चाहिए:** node loaded chain की जानकारी और height दिखाकर सामान्य रूप से exit करे। गलत या missing pin पर non-zero exit आए। गलत pin को ठीक करने के लिए अंदाज़ा न लगाएं—step 4 वाला commitment इस्तेमाल करें।

## 6. दूसरे node से sync करें (एक ही कंप्यूटर)

दो अलग terminals खोलें। Terminal 1 में peer server:

    ./build/taznode --chain local-chain.json --expect-genesis GENESIS_HASH --serve 19001

**क्या दिखना चाहिए:** node 127.0.0.1:19001 पर peers का इंतज़ार करे।

Terminal 2 में उसी chain की अलग copy बनाकर peer चलाएं:

    cp local-chain.json peer-chain.json
    ./build/taznode --chain peer-chain.json --expect-genesis GENESIS_HASH --peer 19001

Windows UCRT64 में copy के लिए cp उपलब्ध है; Windows Command Prompt में copy local-chain.json peer-chain.json चलाएं और फिर node command चलाएं।

**क्या दिखना चाहिए:** peer connect/sync का status और सफल completion। दोनों chain files का genesis pin एक होना चाहिए। इस prototype में peer केवल localhost पर है; दो अलग कंप्यूटरों के बीच connection नहीं चलेगा।

## 7. Sync/height देखें

हर बार node को सही pin के साथ खोलें:

    ./build/taznode --chain local-chain.json --expect-genesis GENESIS_HASH
    ./build/taznode --chain peer-chain.json --expect-genesis GENESIS_HASH

**क्या दिखना चाहिए:** दोनों में समान chain का genesis commitment और synced height दिखे। कोई block बना/आया नहीं है तो height समान रहना ही अपेक्षित है। यह production network की सुरक्षा या स्थिरता का प्रमाण नहीं है।

## आम गलतियाँ

- **Fee 1000:** prototype transfer fee 1000 utzv है। transfer के लिए sender के पास रकम के साथ fee भी होनी चाहिए; यह fee नीति final नहीं है।
- **Genesis pin गलत:** दोनों nodes में एक ही सही commitment दें। अलग genesis वाली chain sync नहीं होगी।
- **Port busy:** --serve 19001 पर bind error आए तो उस port पर पहले से process है। server बंद करें या नया unused port चुनें; peer command में वही port दें।
- **पुराना binary:** हर source update के बाद build दोबारा करें: cmake --build build --parallel. चल रहे पुराने process को बंद करके नया binary शुरू करें।
- **Compiler/OpenSSL नहीं मिला:** सही MSYS2 UCRT64 terminal या supported compiler/OpenSSL development package इस्तेमाल करें।
- **पुरानी chain file:** genesis बनाने की command existing file overwrite नहीं करती। नया filename दें या केवल disposable local test data हटाने के बाद दोबारा चलाएं।
