# दो मशीनों पर TAZVERA node चलाने की गाइड

यह गाइड घर के एक ही Wi-Fi/LAN पर दो कंप्यूटरों के बीच **testnet sync** जाँचने के लिए है। इसे तभी चलाएँ जब आपके checkout में नीचे बताए गए network flags merge होकर उपलब्ध हों। Testnet में असली पैसा, वास्तविक भुगतान, TLS या production consensus नहीं है। सार्वजनिक इंटरनेट पर port न खोलें।

## पहले यह समझें

- **मशीन A:** Windows laptop/PC — server node।
- **मशीन B:** दूसरा laptop/PC — peer node; Windows या Linux हो सकता है।
- दोनों एक ही Wi-Fi router/network पर हों। Guest Wi-Fi या client/AP isolation वाला network दो devices को एक-दूसरे से रोक सकता है।
- नीचे `PORT` को दोनों commands में एक ही port से बदलें। `A_LAN_IP` को मशीन A के LAN IPv4 address से बदलें।
- `--peer <A का LAN पता>:<port>` flag अभी बन रहा है। **जब यह flag आ जाए**, तभी B वाला peer command चलाएँ। अभी मौजूद न हो तो इसे चलाने की कोशिश न करें; code merge होने के बाद इस guide के flag syntax की पुष्टि करें।

## 1. दोनों मशीनें एक ही Wi-Fi पर हैं या नहीं

**Windows (A और B):**

1. Taskbar में Wi-Fi icon दबाएँ।
2. दोनों कंप्यूटरों पर जुड़े Wi-Fi network/SSID का नाम देखें।
3. सुनिश्चित करें कि दोनों एक ही घर के router के उसी network पर हैं। Guest network से बचें।

**Linux (जिस मशीन पर Linux है):**

- Wi-Fi menu में connected network का नाम देखें।
- Terminal में `ip addr` चलाकर Wi-Fi interface का IPv4 address भी देखें।

## 2. मशीन A का LAN पता निकालें

**Windows A:**

1. Start खोलें, `cmd` लिखें और Command Prompt खोलें।
2. चलाएँ:

       ipconfig

3. Wi-Fi adapter का section ढूँढें। **IPv4 Address** लिख लें, उदाहरण के लिए `192.168.1.25`। VPN, VirtualBox या अन्य adapter का पता न लें।

**Linux A (यदि A पर Linux हो):**

1. Terminal खोलें और चलाएँ:

       ip addr

2. connected Wi-Fi interface (अक्सर `wlan0` या `wlp...`) के `inet` address को लें। `127.0.0.1` नहीं लेना है।

इसी guide में उस address को `A_LAN_IP` कहा गया है। Router से मिलने वाला address बाद में बदल सकता है; हर नए test से पहले फिर जाँचें।

## 3. दोनों पर एक जैसी genesis chain बनाएं

Genesis एकदम समान होना चाहिए। दोनों मशीनों पर **वही दो account addresses, वही रकम, वही क्रम और वही genesis options** इस्तेमाल करें। केवल अलग-अलग key बनाकर अलग address इस्तेमाल करेंगे तो genesis pin अलग होगा।

1. मशीन A पर node की वर्तमान instructions के अनुसार दो test addresses तैयार करें। दोनों पूरे addresses copy करें।
2. उन addresses और balances को मशीन B तक सुरक्षित तरीके से पहुँचाएँ। ये test-only public addresses हों; private key/seed कभी साझा न करें।
3. दोनों मशीनों पर अलग-अलग खाली chain file बनाएँ और समान genesis command चलाएँ। उदाहरण में placeholders को अपने वास्तविक test addresses से बदलें:

       ./build/taznode --chain two-machine.json --init-genesis ADDRESS_A:100000000 --init-genesis ADDRESS_B:100000000

   Windows पर `./build/taznode` की जगह `build/taznode.exe` लिखें। अगर वर्तमान binary का नाम/path अलग है तो उसी build की वास्तविक command लें।
4. दोनों outputs में छपा genesis commitment/pin नोट करें। **A और B का pin बिल्कुल एक जैसा होना चाहिए।** अलग हो तो आगे न बढ़ें—addresses, रकम, क्रम और options मिलाकर फिर से नई disposable chain files बनाएँ।
5. दोनों पर वही pin `--expect-genesis` से दें। किसी pin का अंदाज़ा न लगाएँ।

## 4. Windows Firewall और Private network

Windows का Firewall incoming connection रोक सकता है, भले दोनों मशीनें एक ही Wi-Fi पर हों। Network profile को **Private** तभी करें जब यह आपका भरोसेमंद घर/निजी network हो। Public Wi-Fi को Private न बनाएं।

### मशीन A — incoming TCP port allow करें

1. Windows Settings → Network & Internet → Wi-Fi → connected network की Properties खोलें।
2. Network profile में **Private** चुनें, केवल भरोसेमंद घर के Wi-Fi के लिए।
3. Start में **Windows Defender Firewall with Advanced Security** खोजकर खोलें।
4. **Inbound Rules** → **New Rule…** → **Port** → TCP चुनें।
5. **Specific local ports** में वही `PORT` लिखें जो node के `--serve` में देंगे।
6. **Allow the connection** चुनें। Profile में केवल **Private** टिक रखें।
7. नाम दें, जैसे `TAZVERA testnet TCP PORT`, फिर Finish करें।

यह rule केवल उस TCP port के लिए है; Firewall पूरी तरह बंद न करें। Test खत्म होने पर इस rule को disable/delete कर दें।

### मशीन B — outbound connection

1. B पर भी Windows Settings → Network & Internet → Wi-Fi → Properties में भरोसेमंद घर के network को **Private** करें।
2. सामान्य Windows Firewall में outbound connections default रूप से allowed होते हैं, इसलिए B पर आम तौर पर inbound port rule की जरूरत नहीं होती।
3. यदि B पर outbound traffic रोकने वाली custom policy है, तो Windows Defender Firewall with Advanced Security → **Outbound Rules** → **New Rule…** → **Port** → TCP → उसी `PORT` पर allow rule बनाएँ। जहाँ संभव हो remote address को केवल `A_LAN_IP` तक सीमित रखें।

यदि B Linux है, तो उसके firewall में outbound TCP को A के LAN IP और उसी port तक अनुमति दें। Linux A होने पर उसके firewall में उसी port का inbound TCP अनुमति दें।

## 5. पहले ping, फिर port जाँचें

**B से A को ping करें:**

- Windows B: Command Prompt में `ping A_LAN_IP` चलाएँ।
- Linux B: Terminal में `ping -c 4 A_LAN_IP` चलाएँ।

उदाहरण में `A_LAN_IP` को वास्तविक पता दें। Ping का जवाब मिलना network reachability का संकेत है, लेकिन यह साबित नहीं करता कि node का TCP port खुला है। Ping fail होने पर Wi-Fi/SSID, IP address, subnet, guest network और router का client/AP isolation देखें। Windows Firewall ICMP ping को भी रोक सकता है।

**Ping चलता है लेकिन TCP port नहीं चलता:**

- A पर node सच में चल रहा है और `--serve PORT --listen-on A_LAN_IP` के साथ उसी port पर listen कर रहा है, यह जाँचें।
- A की inbound firewall rule में TCP, सही port और Private profile जाँचें।
- B का peer command सही A address और वही port इस्तेमाल कर रहा है, यह जाँचें।
- दोनों मशीनों की clock या genesis pin से पहले network समस्या न मानें; पहले address/port और firewall की पुष्टि करें।
- Router में AP/client isolation, guest Wi-Fi, VPN या third-party firewall connection रोक सकता है।
- Port को internet/router पर forward न करें। यह guide केवल उसी भरोसेमंद LAN के लिए है।

## 6. Node चलाएँ और sync देखें

पहले A पर node executable का path और flags अपने current build के अनुसार जाँचें। `--listen-on` और `--produce-every` अभी development में हो सकते हैं; इन्हें केवल merge होने और `--help` में उपलब्ध होने के बाद इस्तेमाल करें।

**मशीन A — server:**

       ./build/taznode --chain two-machine.json --expect-genesis GENESIS_PIN --serve PORT --listen-on A_LAN_IP

- `GENESIS_PIN` को दोनों पर समान बने pin से बदलें।
- `PORT` को चुने हुए port से और `A_LAN_IP` को A के Wi-Fi IPv4 address से बदलें।
- Windows पर executable path `build/taznode.exe` हो सकता है।
- A की terminal खुली रखें। Expected log में server/listen address और port दिखना चाहिए।

**मशीन B — peer (जब यह flag आ जाए):**

       ./build/taznode --chain two-machine.json --expect-genesis GENESIS_PIN --peer A_LAN_IP:PORT

यह `--peer` syntax अभी बन रहा है; merge के बाद वास्तविक flag नाम और arguments `--help`/updated runbook से मिलाएँ। अनुमान से flag न चलाएँ।

**Sync की पुष्टि:**

- B की terminal में connection और sync के logs देखें।
- B पर chain height बढ़नी चाहिए, जब A के पास B से आगे के blocks हों।
- A पर `sent N blocks` जैसा log दिखना चाहिए, जब A ने B को blocks भेजे हों। Exact wording build के अनुसार बदल सकती है।
- दोनों पर genesis pin समान होना चाहिए। अगर A के पास भेजने के लिए कोई नया block नहीं है, तो height बढ़ना या `sent N blocks` दिखना जरूरी नहीं।
- Sync पूरा होने पर दोनों nodes को साफ़ तरीके से रोकें और logs सुरक्षित रखें।

## 7. आम errors का मतलब

Exact error text build के अनुसार बदल सकता है। नीचे अर्थ और अगली जाँच दी है।

| Error / लक्षण | मतलब और क्या जाँचें |
|---|---|
| `connection refused` | A पर node नहीं चल रहा, गलत port/address है, या उस address पर listener नहीं है। A का server log और command जाँचें। |
| `connection timed out` / कोई जवाब नहीं | Firewall, गलत LAN IP, अलग/guest Wi-Fi, AP isolation या network route connection रोक रहा हो सकता है। |
| `address already in use` / bind failed | A पर port पहले से किसी process के पास है। पुराना node बंद करें या unused port चुनें; दोनों commands में वही नया port रखें। |
| `permission denied` / access denied | Firewall rule, Windows profile, Linux firewall या OS permission जाँचें। Firewall पूरी तरह बंद न करें। |
| `peer did not prove an identity` / authentication failed | Peer identity/auth handshake पूरा नहीं हुआ। दोनों sides के logs, सही binary और compatible code version जाँचें। |
| `wrong chain` / genesis mismatch | दोनों genesis pin या genesis inputs अलग हैं। समान addresses, amounts, क्रम और options से नई test chain बनाएँ। |
| Sync शुरू नहीं होता / B की height नहीं बढ़ती | A के पास नए blocks न हों, peer connection न बना हो, या sync request fail हुई हो सकती है। दोनों logs और A/B की heights देखें। |
| A पर `sent N blocks` नहीं दिखता | A ने उस समय blocks नहीं भेजे, या B ने request नहीं की/connection नहीं बना। इसे अकेले failure का निर्णायक प्रमाण न मानें। |
| Chain file parse/load error | File खराब, अधूरी या गलत format की हो सकती है। Node रोकें, file की copy/log सुरक्षित करें; केवल disposable test data होने की पुष्टि के बाद नई chain बनाएँ। |
| Flag unknown / unrecognized option | उस flag का बदलाव अभी merge नहीं हुआ या यह binary पुराना है। वर्तमान `--help` देखें और सही commit से rebuild करें। |

## सुरक्षा याद रखें

- यह LAN testnet है: असली पैसा नहीं, वास्तविक payment नहीं, TLS नहीं और production consensus/finality नहीं।
- Test-only addresses, keys और chain files इस्तेमाल करें। Private keys साझा न करें।
- Test पूरा होने पर server बंद करें और बनाई गई temporary firewall rules हटाएँ।
