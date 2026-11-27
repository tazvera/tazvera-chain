# टेस्टनेट नोड चलाने की गाइड
<small>Testnet node operations guide</small>

**यह गाइड चल रहे टेस्टनेट नोड में आम समस्याओं को पहचानने और सुरक्षित तरीके से संभालने के लिए है।**  
<small>Use this guide to diagnose common testnet node problems and recover safely.</small>

**सबसे पहले:** कोई भी समस्या आए तो नोड की स्क्रीन/लॉग, समय, chain file का path और आख़िरी सफल height नोट करें।  
<small>First record the logs, time, chain-file path, and last known height.</small>

**चेन file को हाथ से edit, delete या दूसरी मशीन की file से replace न करें।**  
<small>Never manually edit, delete, or replace a chain file.</small>

**टेस्टनेट में data बचाना भी ज़रूरी है; recovery से पहले नोड को साफ़ तरीके से रोकें और file की backup copy रखें।**  
<small>Stop cleanly and back up before recovery.</small>

**ये कदम मौजूदा prototype के लिए हैं; यह production recovery procedure नहीं है।**  
<small>These steps are for the prototype, not production recovery.</small>

## 1. Block बनना रुक गया — height नहीं बढ़ रही
<small>Block production stopped — height is not increasing</small>

**लक्षण**
- Height कुछ समय से वही है।
- नया block नहीं दिख रहा या producer की activity बंद है।

**संभावित कारण और क्या करें**
- **Producer बंद है या node केवल sync/listen mode में है:** process और startup command देखें। अगर producer अलग से चलाया जाता है, उसकी स्थिति और logs जाँचें। Producer चालू करने के लिए सिर्फ़ वही documented command इस्तेमाल करें जो आपके binary में मौजूद है; अनुमान से नया flag न जोड़ें।
- **मशीन की clock पीछे है:** सिस्टम का समय और timezone जाँचें। Clock सही करें (नीचे समस्या 2 देखें), फिर node/producer के logs देखें।
- **Disk भर गई है:** chain file जिस drive पर है वहाँ free space देखें। सुरक्षित जगह खाली करें; chain file को न हटाएँ। फिर node को cleanly रोककर restart करें और logs में write error देखें।
- **दूसरा node आगे है या network sync रुका है:** peer connection, peer logs और दोनों nodes की height/hash compare करें। Fork का शक हो तो समस्या 4 के कदम अपनाएँ।

Height का न बढ़ना अकेले यह साबित नहीं करता कि chain खराब है। पहले producer, clock, disk और peer स्थिति जाँचें।

## 2. “timestamp beyond this clock” जैसी चेतावनी
<small>Timestamp is ahead of this node’s clock</small>

**लक्षण:** node किसी block का timestamp अपनी मशीन की मौजूदा घड़ी से आगे बताकर उसे स्वीकार नहीं करता या warning देता है।

**कारण:** दो मशीनों की system clock में फ़र्क़ हो सकता है—एक मशीन तेज़ चल रही हो, दूसरी पीछे हो, या timezone/NTP configuration गलत हो।

**क्या करें**
1. दोनों मशीनों पर तारीख, समय और timezone देखें। केवल timezone बदलने से UTC समय ठीक हो यह ज़रूरी नहीं—घड़ी का वास्तविक समय भी जाँचें।
2. दोनों मशीनों पर automatic time synchronization चालू करें (Windows: Settings → Time & language → Date & time → Set time automatically → Sync now; Linux: अपने system का NTP/chrony service जाँचें)।
3. कुछ समय बाद logs दोबारा देखें। फिर भी warning आए तो दोनों मशीनों के UTC समय और block timestamp को नोट करें; block या chain file को हाथ से बदलकर warning हटाने की कोशिश न करें।
4. जिस node की घड़ी गलत थी, उसे सही समय पर लाकर normal sync/restart करें। पुराने block को manually rewrite न करें।

## 3. Node exit code 3 से बंद हो गया — chain file लिख नहीं रही
<small>Node exited with code 3 — chain file could not be written</small>

**लक्षण:** process बंद हो जाता है और terminal में `cannot write`, `cannot load` या file-related error दिख सकता है।

**ध्यान दें:** exit code 3 केवल disk-write failure का संकेत नहीं है। मौजूदा node startup में chain load/refusal errors भी code 3 दे सकते हैं। Terminal का पूरा error message पढ़ें।

**संभावित कारण**
- Disk या volume भर गया है।
- Node के पास उस folder में write permission नहीं है।
- File किसी दूसरे process, backup tool या sync tool द्वारा lock की गई है।
- Path गलत है, drive unmounted है, या filesystem read-only हो गया है।
- Chain file unreadable/corrupt है या trusted genesis pin उससे मेल नहीं खाता।

**क्या करें**
1. पूरा error message और chain file का exact path लिख लें।
2. Free disk space, folder permission, mounted drive और file lock जाँचें। यदि कोई backup/sync tool file को पकड़े है, उसे सुरक्षित रूप से पूरा/रोकें।
3. Node को cleanly रोकें। Chain file को text editor में खोलकर सुधारना, truncate करना, delete करना या दूसरी file से overwrite करना **न करें**।
4. Storage/permission समस्या ठीक करने के बाद, backup copy सुरक्षित रखकर node को उसी trusted chain file और genesis pin के साथ दोबारा शुरू करें।
5. Error “wrong chain/genesis” या load/parse failure हो तो file बदलने के बजाय सही trusted pin/path की पुष्टि करें। संदेह हो तो आगे लिखना रोकें और maintainer को logs दें।

## 4. दो node एक ही chain पर block बना रहे हैं — fork race
<small>Two nodes produce blocks on the same chain — fork race</small>

**लक्षण:** दोनों nodes की height अलग हो जाती है, या height समान होने पर latest block hash अलग होता है। दोनों की logs में एक ही समय के आसपास अलग blocks बनते दिख सकते हैं।

**कारण:** दो producers एक ही chain state से block बनाने की कोशिश कर रहे हैं। यह race अलग-अलग valid-looking branches बना सकती है; सिर्फ़ सबसे ऊँची height देखकर सही branch तय न करें।

**क्या करें**
1. दोनों producers को रोकें; chain files को न हटाएँ और न एक-दूसरे पर copy करें।
2. दोनों nodes की height, latest block hash, genesis commitment और logs सुरक्षित रखें। किस block से divergence शुरू हुई, यह पता करें।
3. Network/peer sync की स्थिति जाँचें। किसी एक branch को चुनने के लिए केवल documented fork-selection/recovery procedure या maintainer की पुष्टि इस्तेमाल करें।
4. जब तक canonical branch स्पष्ट न हो, दोनों files की अलग backup रखें और किसी node से नए blocks न बनवाएँ।
5. चुनी गई branch पर लौटने का तरीका उपलब्ध न हो तो chain files को manually edit न करें; maintainer को दोनों hashes और logs भेजें।

## 5. पुरानी chain file को नए binary से खोलना
<small>Opening an old chain file with a newer binary</small>

**लक्षण:** नया binary chain file load करने से मना करता है, version/format/parse error देता है, या trusted genesis commitment मेल नहीं खाता।

**कारण:** protocol version या persisted file format बदल गया हो सकता है। नया binary पुराने data को पढ़ सकेगा, यह अपने-आप में तय नहीं है। Genesis pin mismatch का अर्थ यह भी हो सकता है कि गलत chain file चुनी गई है।

**क्या करें**
1. पुरानी chain file की अलग backup copy रखें; original पर migration की कोशिश न करें।
2. Release notes और migration निर्देश देखें: क्या नया binary पुराने format को support करता है और क्या explicit migration आवश्यक है।
3. वही trusted genesis commitment इस्तेमाल करें जो उस chain के लिए जारी किया गया था। Pin mismatch को bypass न करें।
4. यदि compatible format या migration tool उपलब्ध नहीं है, पुराने binary से backup copy पर chain खोलें और maintainer से migration निर्देश लें। पुराने binary से बनी file को नए binary पर बार-बार लिखने की कोशिश न करें।
5. केवल protocol version बदलने से chain की पहचान/genesis नहीं बदलती; लेकिन file format/validation नियम बदल सकते हैं। Upgrade से पहले compatibility स्पष्ट होना ज़रूरी है।

## 6. Node सही तरीके से बंद करना
<small>How to stop a node safely</small>

**सही तरीका — Ctrl+C**
1. उसी terminal में जाएँ जहाँ node चल रहा है।
2. `Ctrl+C` एक बार दबाएँ और process को बंद होने दें।
3. Terminal में shutdown/exit पूरा होने का इंतज़ार करें। फिर जरूरत हो तो chain file की backup लें।

**गलत तरीके**
- `kill -9` या force-kill से process को अचानक मारना।
- Node के लिखते समय कंप्यूटर को power off करना या plug निकालना।
- Chain file को process चलते हुए copy/replace करना।

अचानक बंद होने पर आख़िरी बदलाव disk पर पूरा न लिखा गया हो सकता है। इससे हर बार corruption होगी ऐसा नहीं, लेकिन recovery की गारंटी भी नहीं है। Restart पर error आए तो file को हाथ से ठीक न करें—पहले backup रखें और logs के साथ मदद लें।

---

## छोटी जाँच सूची
<small>Quick checklist</small>

- [ ] सही node/binary और सही chain file चल रही है।
- [ ] Producer चालू है; clock synchronized है।
- [ ] Disk में जगह है और process को file पर सही permission है।
- [ ] दोनों nodes का genesis commitment एक जैसा है।
- [ ] Height के साथ latest block hash भी compare किया है।
- [ ] Shutdown Ctrl+C से किया और chain file की backup सुरक्षित रखी है।
