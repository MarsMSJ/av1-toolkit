# Example: DPB Management Walkthrough (8 Frames)

This walkthrough illustrates how the **Decoded Picture Buffer (DPB)** is managed as Access Units (OBUs) are parsed and frames are decoded.  

Assumptions:
- `enable_order_hint = 1`
- `OrderHintBits = 3` (wrap at 8, so valid order hints are 0..7)
- `ref_frame_idx[7]` is signaled explicitly (no short signaling)
- DPB has 8 slots (`slot[0..7]`) storing `{valid, order_hint}`
- A no-show ALTREF frame is inserted to exercise both forward and backward references

---

## Step 0 — Initial DPB State
- All slots invalid

---

## Frame 0 (decode #0)
- **Type:** KEY, `show_frame=1`, `order_hint=0` → `OH(0)`  
- **Refresh:** Key + show forces `refresh_frame_flags = allFrames`  
- **Resulting DPB:** slots 0–7 refreshed with `OH(0)` and marked valid

---

## Frame 1 (decode #1)
- **Type:** INTER show, `OH(1)`  
- **Refs:** `[0,1,2,3,4,5,6]` → all point to `OH(0)`  
- **Sign-bias:** all forward (`get_relative_dist(0,1) < 0`)  
- **Refresh:** `rff = 0b00000001` → refresh slot0 with `OH(1)`  
- **Resulting DPB:** slot0=`OH(1)`, slots1–7=`OH(0)`

---

## Frame 2 (decode #2)
- **Type:** INTER show, `OH(2)`  
- **Refs:** LAST=slot0 (`OH(1)`), others still point to `OH(0)`  
- **Sign-bias:** all forward  
- **Refresh:** `rff = 0b00000010` → refresh slot1 with `OH(2)`  
- **Resulting DPB:** slot0=`OH(1)`, slot1=`OH(2)`, others=`OH(0)`

---

## Frame 3 (decode #3)
- **Type:** INTER show, `OH(3)`  
- **Refs:** LAST=slot1 (`OH(2)`), GOLD=slot0 (`OH(1)`)  
- **Sign-bias:** all forward  
- **Refresh:** `rff = 0b00000100` → refresh slot2 with `OH(3)`  
- **Resulting DPB:** slot2=`OH(3)`, slot1=`OH(2)`, slot0=`OH(1)`

---

## Frame 4 (decode #4) — No-Show ALTREF
- **Type:** INTER no-show, `show_frame=0`, `order_hint=7` (future display)  
- **Refs:** e.g. slot2 (`OH(3)`) and others  
- **Sign-bias:** relative to `OH(7)`, earlier hints are forward (bias 0)  
- **Refresh:** `rff = 0b10000000` → refresh slot7 with `OH(7)`  
- **Resulting DPB:** slot7=`OH(7)` (backward ref), others unchanged

---

## Frame 5 (decode #5)
- **Type:** INTER show, `OH(4)`  
- **Refs:** LAST=slot2 (`OH(3)`), ALTREF=slot7 (`OH(7)`)  
- **Sign-bias:**  
  - LAST (`OH(3)` vs `OH(4)`): forward (bias 0)  
  - ALTREF (`OH(7)` vs `OH(4)`): backward (bias 1)  
- **Refresh:** `rff = 0b00001000` → refresh slot3 with `OH(4)`  
- **Resulting DPB:** slot3=`OH(4)`, slot7=`OH(7)` kept

---

## Frame 6 (decode #6)
- **Type:** INTER show, `OH(5)`  
- **Refs:** LAST=slot3 (`OH(4)`), ALTREF=slot7 (`OH(7)`)  
- **Sign-bias:** ALTREF backward (bias 1), others forward  
- **Refresh:** `rff = 0b00010000` → refresh slot4 with `OH(5)`  
- **Resulting DPB:** slot4=`OH(5)`, slot7=`OH(7)` retained

---

## Frame 7 (decode #7)
- **Type:** INTER show, `OH(6)`  
- **Refs:** LAST=slot4 (`OH(5)`), ALTREF=slot7 (`OH(7)`)  
- **Sign-bias:** ALTREF backward (bias 1), others forward  
- **Refresh:** `rff = 0b00100000` → refresh slot5 with `OH(6)`  
- **Resulting DPB:** slot5=`OH(6)`, slot7=`OH(7)` still available for `show_existing_frame`  

---

# Key Points Demonstrated
- **Key + show** → refresh all slots  
- **Refresh flags** update only the selected slots; others persist  
- **No-show frames** (e.g., ALTREF) still refresh a slot but are not output immediately  
- **Sign-bias** is determined by `get_relative_dist(ref_hint, curr_hint)`:
  - ≤0 → forward (bias 0)  
  - >0 → backward (bias 1)  
- **ALTREF usage** demonstrates backward referencing in display order  
