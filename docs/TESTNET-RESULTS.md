# पहला सफल local run, 27 नवंबर 2026
# First successful local run, 27 November 2026

यह दस्तावेज़ उपयोगकर्ता द्वारा दिए गए वास्तविक local-run आँकड़े दर्ज करता है। नीचे दिए गए आँकड़े और terminal/API संदेश बिना बदलाव के रखे गए हैं।
This record preserves the run figures and terminal/API messages exactly as supplied by the user.

**सुरक्षा:** इसमें केवल public account addresses हैं। कोई private key या seed दर्ज नहीं है और कभी दर्ज नहीं किया जाएगा।
**Security:** Only public account addresses are included. No private key or seed is recorded or will ever be recorded.

## चेन की पहचान
## Chain identity

- Genesis pin: `5f3ac368be19136baefb2ae8f8bcbb3c23de86a9a6012f1dcfc6485a8c246a63`
- `chain_id`: `tazvera-local-prototype-v1`
- Protocol: `3`

## खाते
## Accounts

केवल पते और genesis balances:
Addresses and genesis balances only:

- k1: `c68b2d0cb47a57e30834fc235b8422bfdc09d728` — genesis में `10,00,000`
- k2: `f46c08954f95a092e225d33d9b3ee379c05c14b7` — genesis में `0`

## Producer node

कमांड:
Command:

```sh
taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --serve 7001 --listen-on 0.0.0.0 --produce-every 5
```

- 131 block बनाए, बिना किसी हाथ के
- चेतावनी छपी: "listening on 0.0.0.0 makes this node reachable beyond this machine; is not encrypted"
- नोट छपा: "this chain declares no producers, so blocks are unsigned"

## Follower node

कमांड:
Command:

```sh
taznode --chain n2.json --expect-genesis <pin> --peer 7001
```

- height `0 → 28`, "accepted 28 block(s)"
- "peer is anonymous (no identity proof)"

## टोकन भेजना
## Token transfer

कमांड:
Command:

```sh
taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --transfer f46c08954f95a092e225d33d9b3ee379c05c14b7:1000
```

- block height 131 बना, एक transaction के साथ
- नतीजा API से: k1 = balance 998000, nonce 1 | k2 = balance 1000, nonce 0
- यानी 1000 भेजे + 1000 fee = 2000 कटे ✓

## क्या साबित हुआ
## What this demonstrates

1. block अपने आप बनते हैं।
   Blocks are produced automatically.
2. दूसरा नोड sync करता है।
   A second node synchronizes.
3. चेन फ़ाइल में सहेजी जाती है (बंद करके खोलने पर वही height)।
   The chain is persisted to a file; reopening it retains the same height.
4. API से balance/nonce पढ़ा जाता है।
   Balance and nonce can be read through the API.
5. टोकन भेजना, fee कटना, nonce बढ़ना — तीनों सही।
   Token transfer, fee deduction, and nonce increment all work as observed.

## क्या साबित नहीं हुआ
## What this does not demonstrate

- दोनों नोड एक ही मशीन पर थे (0.0.0.0 से loopback के रास्ते)।
  Both nodes ran on the same machine, through loopback via 0.0.0.0.
- दो अलग मशीनें, firewall का दूसरा हिस्सा, और अलग OS — तीनों बाक़ी।
  Two separate machines, the other side of a firewall, and a different OS remain untested.
- peer ने अपनी पहचान साबित नहीं की (anonymous)।
  The peer did not prove its identity (anonymous).
- बातचीत unencrypted (TLS नहीं)।
  Communication was unencrypted; TLS was not used.

यह local prototype run है; इससे अलग मशीनों पर networking, peer authentication, TLS, consensus/finality या production readiness साबित नहीं होती।
This is a local prototype run; it does not establish cross-machine networking, peer authentication, TLS, consensus/finality, or production readiness.

## इसे दोहराने का तरीक़ा
## How to reproduce

यह क्रम उसी run का producer → follower → transfer क्रम रखता है। पहले से तैयार local test chain और उसी के लिए बने test key file की ज़रूरत है। `<pin>` को ऊपर दिए गए genesis pin से बदलें। Private key file को साझा या docs में न जोड़ें।
This follows the same producer → follower → transfer order. It assumes the local test chain and its matching test key file already exist. Replace `<pin>` with the genesis pin above. Do not share or add the private key file to the docs.

1. Producer terminal में:
   In the producer terminal:

   ```sh
   taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --serve 7001 --listen-on 0.0.0.0 --produce-every 5
   ```

2. उसी मशीन पर दूसरे terminal में follower चलाएँ:
   In a second terminal on the same machine, start the follower:

   ```sh
   taznode --chain n2.json --expect-genesis <pin> --peer 7001
   ```

3. Producer chain पर transfer चलाएँ:
   Run the transfer against the producer chain:

   ```sh
   taznode --chain n1.json --expect-genesis <pin> --key-file k1.key --transfer f46c08954f95a092e225d33d9b3ee379c05c14b7:1000
   ```

4. API से दोनों accounts के balance और nonce पढ़ें। अपेक्षित परिणाम इसी दर्ज run के अनुसार: k1 balance `998000`, nonce `1`; k2 balance `1000`, nonce `0`. यह reproduction निर्देश है, भविष्य के हर run के परिणाम की गारंटी नहीं।
   Read both accounts' balance and nonce through the API. The expected result for this recorded run is: k1 balance `998000`, nonce `1`; k2 balance `1000`, nonce `0`. This is a reproduction guide, not a guarantee for every future run.

**सुरक्षा नोट:** इस run में `0.0.0.0` listener और unsigned blocks की चेतावनियाँ दर्ज हुईं। इसे public network पर न चलाएँ; इसमें TLS या peer identity proof नहीं था।
**Security note:** This run logged the `0.0.0.0` listener and unsigned-block warnings. Do not run it on a public network; it had neither TLS nor peer identity proof.
