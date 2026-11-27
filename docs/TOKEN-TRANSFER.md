# टोकन भेजना और लेना
<small>Sending and receiving tokens</small>

## टोकन क्या है?
<small>What is a token?</small>

टोकन चेन की किताब में दर्ज एक संख्या है। यह अपने-आप असली पैसा नहीं है और इसकी कोई तय नकद कीमत नहीं मानी जा सकती। यह दस्तावेज़ मौजूदा TAZVERA prototype के परीक्षण के लिए है।

## आज टोकन कैसे भेजें?
<small>How to send tokens today</small>

अभी टोकन भेजने का तरीका कमांड लाइन है। भेजने वाले की key file और सही chain का genesis pin चाहिए। नीचे `<पता>` की जगह पाने वाले का पूरा 40-character hexadecimal address और `<pin>` की जगह भरोसेमंद genesis pin डालें।

```sh
taznode --chain c1.json --expect-genesis <pin> --key-file a.key --transfer <पता>:1000
```

यह उदाहरण 1000 token units भेजता है। मौजूदा CLI एक transfer के लिए एक block बनाता है और फिर रुक जाता है। यह prototype command है—कोई public payment service नहीं।

## Fee 1000 क्यों कटती है?
<small>Why is a 1000-unit fee charged?</small>

हर transaction पर 1000 units की fee लगती है। भेजने वाले के खाते से भेजी गई रकम **और** 1000 fee दोनों कटते हैं। Fee को जला दिया जाता है; वह पाने वाले को नहीं मिलती। इसका उद्देश्य नेटवर्क को बेकार या spam transactions से बचाने में मदद करना है। यह मौजूदा prototype की fee है, अंतिम आर्थिक नियम नहीं।

उदाहरण: 5000 units भेजने के लिए balance कम-से-कम 6000 units होना चाहिए।

## भेजने के बाद क्या होता है?
<small>What happens after sending?</small>

कमांड transaction को block में रखती है। दूसरा node block sync करके उसे जाँचता और अपनी chain में स्वीकार करता है। यह prototype है; block बनना अपने-आप में किसी भुगतान की कानूनी या आर्थिक पुष्टि नहीं है।

## Balance कैसे देखें?
<small>How to check a balance</small>

Node पर read-only API चालू करें:

```sh
taznode --chain c1.json --expect-genesis <pin> --api 8080
```

फिर browser या HTTP client से यह GET request करें:

```http
GET /balance/<पता>
```

उदाहरण URL: `http://127.0.0.1:8080/balance/<पता>`

उत्तर में balance और अगला nonce मिलता है:

```json
{"balance":"10000","nonce":"0"}
```

यहाँ `10000` और `0` केवल उदाहरण हैं। मौजूदा API loopback पर read-only है; यह transaction भेजने का endpoint नहीं है।

## आम गलतियाँ
<small>Common errors</small>

- **गलत पता:** address ठीक 40 hexadecimal characters का होना चाहिए। API में गलत address पर error आता है: `address must be exactly 40 hexadecimal characters`. Transfer command में गलत/अधूरा address देने पर usage error आता है।
- **Fee सहित balance कम:** `insufficient balance`. भेजी जाने वाली रकम और 1000 fee—दोनों के लिए balance चाहिए।
- **Nonce गलत:** `bad nonce`. Sender का nonce chain में मौजूद अगले nonce के बराबर होना चाहिए। Balance API का `nonce` देखें।
- **गलत chain:** `wrong chain ID`. Transaction और node एक ही chain के होने चाहिए; सही chain file और trusted genesis pin इस्तेमाल करें।

## बहुत ज़रूरी चेतावनी
<small>Important warning</small>

पता भेजने से पहले दो बार जाँचें। गलत पते पर भेजा गया टोकन वापस नहीं आता—इसे असली chain की तरह ही समझें। Private key file किसी को न दें।
