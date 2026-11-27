# Explorer check — asset owner after recovery

## Finding (main snapshot)

The Explorer's verification model does apply recovery to its in-memory asset registry: in `explorer/script.src.js`, `beginBlock` moves each asset owned by the recovered address to the recovery target when the recovery becomes due.

However, the visible transaction card is not a current asset-registry view. For kind 6/7, `blockCard` displays transaction fields (the owner encoded in that historical transaction, recipient, and asset ID). The current Explorer UI does not render a post-recovery asset registry row that lets a user confirm the asset's current owner after recovery.

**Result: not visibly verified in the Explorer UI.** The internal simulation changes the owner, but the displayed transaction owner is historical and should not be mistaken for current ownership. Per instruction, this is a report only; no Explorer code was changed.
