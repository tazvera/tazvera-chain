# CHAIN REQUIREMENTS — कोड-जाँच में मिली सुधारें
# Corrections found while checking the master checklist

## 1. Chain ID और genesis commitment

मूल सूची में कहा गया कि chain_id genesis से बंधा नहीं है। यह हिस्सा सही नहीं: genesis_commitment_of(...) में fixed CHAIN_ID शामिल है, और loader file का chain_id तथा pinned genesis commitment जाँचता है। Transaction/hello identity में भी chain ID/genesis commitment शामिल हैं।

सही स्थिति: chain_id genesis commitment से बंधा है, लेकिन अभी compile-time fixed tazvera-local-prototype-v1 है; अलग network के लिए configure करने का विकल्प नहीं। इसलिए checklist में इसे आंशिक लिखा है।

## 2. Peer addressing

--listen-on <IPv4> से node specified address पर inbound connections सुन सकता है, जिसमें 0.0.0.0 भी शामिल है। लेकिन --peer अभी केवल port लेता है और connect_loopback() के जरिए 127.0.0.1 से connect करता है। इसलिए बाहरी inbound सुनना मौजूद है, पूर्ण two-way remote peer addressing नहीं।

## 3. API और Explorer

API read-only है और 127.0.0.1 पर bind होती है; node help में GET-only लिखा है। POST /tx मौजूद नहीं। Explorer चुनी हुई JSON file verify करता है; यह live API explorer नहीं है।

## Scope

यह correction log docs/ में रखा गया है ताकि इस PR में केवल docs/ paths बदलें। Root-level OPEN-DECISIONS.md को इस docs-only PR में नहीं बदला गया।
