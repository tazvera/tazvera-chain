# दो मशीनों पर TAZVERA node चलाने की गाइड

यह गाइड भरोसेमंद, एक ही घर के LAN/Wi-Fi पर testnet की तैयारी के लिए है। अभी **दो अलग मशीनों के बीच peer connection उपलब्ध नहीं है**: `--peer` केवल `127.0.0.1` से जुड़ता है; उसमें remote address देने का विकल्प बाकी है और Claude उसे बना रहा है। तब तक दोनों नोड **एक ही मशीन पर** चलाकर देखें। सार्वजनिक इंटरनेट पर port न खोलें।

## अभी की सीमा

- `--listen-on ADDRESS` मौजूद है और `--serve PORT` के साथ A के LAN interface पर bind करने के लिए है।
- `--peer` अभी केवल localhost port लेता है; `--peer A_LAN_IP:PORT` अभी काम करने वाला remote-peer command नहीं है।
- इसलिए नीचे दिए गए A/B LAN commands **आगे के remote-peer विकल्प के लिए अपेक्षित syntax** हैं, अभी चलने वाले test के निर्देश नहीं। Remote peer address support merge होने और test होने तक localhost test ही करें।

## LAN address कैसे पता करें

Windows A पर Command Prompt में `ipconfig` चलाएँ और Wi-Fi adapter का IPv4 Address लिखें (उदाहरण: `192.168.1.25`)। Linux पर `ip addr` चलाएँ और connected Wi-Fi interface का `inet` address लें। `127.0.0.1`, VPN या virtual adapter का पता न लें। दोनों मशीनें भरोसेमंद उसी LAN पर हों; guest Wi-Fi/client isolation रोक सकता है।

## अपेक्षित दो-मशीन commands — remote `--peer` support आने के बाद

दोनों मशीनों पर compatible, उसी commit से बने binaries और समान genesis pin इस्तेमाल करें। `<pin>` और IP placeholders को वास्तविक values से बदलें।

**मशीन A — server:**

```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001 --listen-on <A का LAN पता>
```

A की terminal खुली रखें। LAN address वही दें जो A के connected LAN interface पर है। Port 7001 उपलब्ध होना चाहिए।

**मशीन B — peer:**

```sh
taznode --chain b.json --expect-genesis <pin> --peer <A का LAN पता>:7001
```

**महत्वपूर्ण:** B से A तक जाने के लिए `--peer` में address वाला विकल्प अभी बाकी है, यह Claude बना रहा है। Remote-peer support merge होने, `--help` में दिखने और test होने से पहले ऊपर की B command न चलाएँ। तब तक दोनों नोड एक ही मशीन पर चलाकर देखें; उस test में server `--serve 7001` और peer `--peer 7001` इस्तेमाल करता है।

## Remote peer support आने पर LAN checks

1. B से A का सही LAN IPv4 address ping करें। Ping failure पर Wi-Fi/SSID, guest network, subnet, VPN और router client/AP isolation जाँचें; ping block होना अकेले TCP failure साबित नहीं करता।
2. A पर node `--serve 7001 --listen-on <A का LAN पता>` के साथ चल रहा हो और log में bind/listen सफल दिखे।
3. A के Windows Firewall में केवल भरोसेमंद Private network पर TCP 7001 inbound allow करें। Firewall पूरी तरह बंद न करें; test के बाद temporary rule हटाएँ।
4. B की command में A का सही LAN IP और वही port दें। Router पर internet port-forwarding न करें।
5. Connection न बने तो क्रम से: node चल रहा है? LAN IP सही है? port समान/free है? firewall rule सही profile/port पर है? binaries एक ही compatible commit के हैं? फिर दोनों logs देखें।

## एक ही मशीन पर अभी का supported test

दो terminals खोलें और chain की अलग copies इस्तेमाल करें।

Server terminal:
```sh
taznode --chain a.json --expect-genesis <pin> --serve 7001
```

Peer terminal:
```sh
taznode --chain b.json --expect-genesis <pin> --peer 7001
```

दोनों terminals पर सफल connection/sync logs देखें। B की height तभी बढ़ेगी जब A के पास B से आगे के blocks हों। दोनों files का genesis pin समान होना चाहिए। Exact executable path आपके build पर निर्भर है; Windows पर सामान्यतः `build/taznode.exe` इस्तेमाल करें।

## सुरक्षा

यह development test है: असली funds/keys इस्तेमाल न करें। TLS अभी wired नहीं है; plaintext traffic को भरोसेमंद LAN तक सीमित रखें। Public Wi-Fi या public internet पर node expose न करें।