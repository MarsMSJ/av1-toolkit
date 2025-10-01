# DPB Walkthrough — Parameters Needed Per Step (DAV1D-style)

This note lists **exact parameters** needed at each step of the 8‑frame walkthrough, using names that appear in **dav1d** public headers (from `headers.h`) where possible. Redundancy is intentional so each step is self-contained.

**Key DAV1D structs/fields referenced** (from `Dav1dSequenceHeader` / `Dav1dFrameHeader`):  
- **Sequence header (`Dav1dSequenceHeader`)**
  - `order_hint` (boolean/flag: tool enable)  
  - `order_hint_n_bits` (number of LSB bits used for order hint wrap)  
- **Frame header (`Dav1dFrameHeader`)**
  - `frame_type` (enum `Dav1dFrameType`)
  - `show_frame` (u8)
  - `show_existing_frame` (u8)
  - `existing_frame_idx` (u8) — index in DPB when `show_existing_frame==1`
  - `refresh_frame_flags` (u8 mask for 8 DPB slots)
  - `frame_offset` (u8) — per-frame order hint (LSB of output order)
  - `temporal_id`, `spatial_id` (not used for DPB mechanics here, but present)
  - Reference signaling:
    - `refs.frame_refs_short_signaling` (u8)
    - `refs.last_frame_idx`, `refs.gold_frame_idx` (when short signaling)
    - `refs.ref_frame_idx[7]` (explicit mapping when not short signaling)
  - Error resilient related (naming may vary internally):
    - `error_resilient_mode` (u8)
    - `ref_order_hint[8]` (signaled in uncompressed header when ER+order_hint; may be internal)

**DPB internal state (decoder-owned, not part of dav1d public header):**
- `slot[i].valid` (u8)
- `slot[i].order_hint` (int)
- `slot[i].slot_id` (u8) (debugging)
- Derived per-frame arrays:
  - `ref_frame_idx[7]` (u8) — indices chosen for LAST..ALTREF
  - `ref_frame_sign_bias[7]` (u8) — forward(0)/backward(1)

---

## Step 0 — Initial DPB State

**Required parameters:**
- From `Dav1dSequenceHeader`: `order_hint`, `order_hint_n_bits`
- DPB: initialize `slot[0..7].valid = 0`, `slot[0..7].order_hint = 0`

**Notes:**
- No `Dav1dFrameHeader` yet; this is pre-roll setup.
- If `order_hint==0`, sign-bias is disabled (all zeros).

---

## Frame 0 (decode #0) — KEY + show, `frame_offset=0` (OH=0)

**From `Dav1dFrameHeader`:**
- `frame_type = KEY_FRAME`
- `show_frame = 1`
- `show_existing_frame = 0`
- `frame_offset = 0` (current OrderHint)
- `refresh_frame_flags = 0xFF` (key+show forces full refresh)

**From `Dav1dSequenceHeader`:**
- `order_hint` (enable flag) = 1
- `order_hint_n_bits = 3` (example assumption)

**Derived / DPB actions:**
- Build refs: n/a for KEY
- Compute sign-bias: n/a (no refs), or treat as all zeros
- Refresh DPB: for `i in 0..7`: `slot[i].valid=1`, `slot[i].order_hint=frame_offset(0)`

---

## Frame 1 (decode #1) — INTER show, `frame_offset=1` (OH=1)

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 1`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7] = [0,1,2,3,4,5,6]`
- `refresh_frame_flags = 0b00000001` (refresh slot0)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Resolve `ref_frame_idx[7]` = as signaled
- For each ref i: `hint = DPB.slot[ ref_frame_idx[i] ].order_hint` (all 0)
- Compute `ref_frame_sign_bias[i]` via `get_relative_dist(hint, frame_offset)` → all forward (0)
- Apply refresh: set `slot0.order_hint = 1` (valid stays 1)

---

## Frame 2 (decode #2) — INTER show, `frame_offset=2` (OH=2)

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 2`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` (keep simple): `[0,1,2,3,4,5,6]` (LAST points to slot0 which is now OH=1)
- `refresh_frame_flags = 0b00000010` (refresh slot1)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- `ref_frame_idx` as signaled
- Sign-bias: all forward (0) (hints ≤ 1 vs current 2)
- Refresh: `slot1.order_hint = 2`

---

## Frame 3 (decode #3) — INTER show, `frame_offset=3` (OH=3)

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 3`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` example: LAST→slot1, GOLDEN→slot0, others arbitrary legal
- `refresh_frame_flags = 0b00000100` (refresh slot2)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Build refs as per explicit array
- Sign-bias: forward (0) for all
- Refresh: `slot2.order_hint = 3`

---

## Frame 4 (decode #4) — INTER **no-show**, `frame_offset=7` (future ALTREF store)

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 0`, `show_existing_frame = 0`
- `frame_offset = 7`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` (e.g., `[0,1,2,3,4,5,6]`)
- `refresh_frame_flags = 0b10000000` (store in slot7)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Refs valid; sign-bias relative to 7 → earlier hints are forward (0)
- No output (since `show_frame=0`)
- Refresh: `slot7.order_hint = 7` (backward ref for subsequent frames)

---

## Frame 5 (decode #5) — INTER show, `frame_offset=4`

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 4`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` with:
  - `ref_frame_idx[LAST] = 2` (slot2, hint=3)
  - `ref_frame_idx[ALTREF] = 7` (slot7, hint=7)
  - others arbitrary/legal
- `refresh_frame_flags = 0b00001000` (refresh slot3)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Sign-bias examples:
  - LAST: `get_relative_dist(3,4) < 0` → forward (0)
  - ALTREF: `get_relative_dist(7,4) > 0` → backward (1)
- Refresh: `slot3.order_hint = 4`

---

## Frame 6 (decode #6) — INTER show, `frame_offset=5`

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 5`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` with:
  - LAST=slot3 (hint=4), ALTREF=slot7 (hint=7)
- `refresh_frame_flags = 0b00010000` (refresh slot4)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Sign-bias: ALTREF backward (1), others forward (0)
- Refresh: `slot4.order_hint = 5`

---

## Frame 7 (decode #7) — INTER show, `frame_offset=6`

**From `Dav1dFrameHeader`:**
- `frame_type = INTER_FRAME`
- `show_frame = 1`, `show_existing_frame = 0`
- `frame_offset = 6`
- `refs.frame_refs_short_signaling = 0`
- `refs.ref_frame_idx[7]` with:
  - LAST=slot4 (hint=5), ALTREF=slot7 (hint=7)
- `refresh_frame_flags = 0b00100000` (refresh slot5)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`, `order_hint_n_bits = 3`

**Derived / DPB actions:**
- Sign-bias: ALTREF backward (1), others forward (0)
- Refresh: `slot5.order_hint = 6`
- Slot7 (`OH=7`) remains available for a later `show_existing_frame`:
  - If a later `Dav1dFrameHeader` sets `show_existing_frame = 1` and `existing_frame_idx = 7`, the loader (§7.21) displays it without decoding a new frame.

---

## Error-Resilient + Order-hint (when present)

**From `Dav1dFrameHeader`:**
- `error_resilient_mode = 1`
- `ref_order_hint[8]` provided in uncompressed header (naming may differ in dav1d internals)

**From `Dav1dSequenceHeader`:**
- `order_hint = 1`

**Derived / DPB actions:**
- For each slot `i`: if `DPB.slot[i].valid` and `DPB.slot[i].order_hint != ref_order_hint[i]`, set `DPB.slot[i].valid = 0` **before** building `ref_frame_idx[7]` and computing sign-bias.

---

## Quick field→purpose cheat sheet (DAV1D names)

- `Dav1dSequenceHeader.order_hint` → enable flag for order-hint tool
- `Dav1dSequenceHeader.order_hint_n_bits` → LSB width for wrap-aware comparisons
- `Dav1dFrameHeader.frame_offset` → current frame’s order hint (LSB bits)
- `Dav1dFrameHeader.show_frame`, `show_existing_frame`, `existing_frame_idx` → output timing and existing-frame display
- `Dav1dFrameHeader.refresh_frame_flags` → DPB overwrite mask
- `Dav1dFrameHeader.refs.*` → reference frame index signaling
- `Dav1dFrameHeader.error_resilient_mode` (+ signaled `ref_order_hint[8]`) → slot invalidation guard
- **DPB.slot[i].order_hint** (decoder state) ↔ used to compute `ref_frame_sign_bias[7]` via `get_relative_dist()`

