# Example: DPB Management Walkthrough (8 Frames) — With Pseudo‑C++ Using DAV1D Headers

This is the same 8-frame walkthrough, but with **pseudo‑C++** that matches typical inputs parsed from **dav1d** sequence and frame headers.
The code is library‑style (no exceptions) and separates **syntax→DPB** responsibilities.

---

## 0) Common Types (sequence / frame inputs and DPB)

```cpp
// ---- Inputs from DAV1D sequence & frame headers (names mirror DAV1D/AOM concepts) ----
enum Av1FrameType : uint8_t { KEY_FRAME, INTER_FRAME, INTRA_ONLY_FRAME, SWITCH_FRAME };

struct Av1Seq {                   // from Dav1dSequenceHeader
    uint8_t enable_order_hint;    // seqhdr.enable_order_hint
    uint8_t order_hint_bits;      // seqhdr.order_hint_n_bits (aka order_hint_bits)
};

struct Av1Refs {                  // source: frame header, either explicit or short signaling derived
    uint8_t frame_refs_short_signaling; // fh->frame_refs_short_signaling
    uint8_t last_frame_idx;       // when short signaling
    uint8_t gold_frame_idx;       // when short signaling
    uint8_t ref_frame_idx[7];     // explicit mapping for LAST..ALTREF
};

struct Av1FrameHdr {              // from Dav1dFrameHeader
    Av1FrameType frame_type;
    uint8_t      show_frame;
    uint8_t      show_existing_frame;
    uint8_t      frame_to_show_map_idx;     // idx in DPB when show_existing_frame==1
    uint8_t      refresh_frame_flags;       // 8-bit mask, which DPB slots to refresh
    uint8_t      error_resilient_mode;      // needed when ref_order_hint[] is signaled
    uint8_t      order_hint;                // LSB(order_hint_bits) of expected output order
    Av1Refs      refs;
    // Optional if signaled in error resilient + order hint mode:
    uint8_t      has_ref_order_hint;        // derived from header signaling
    uint8_t      ref_order_hint[8];         // when present in the bitstream
};

// ---- DPB state ----
struct DpbSlot {
    uint8_t valid;
    uint8_t slot_id;        // 0..7 (redundant but handy when debugging)
    int     order_hint;     // saved OrderHint for that slot
    // NOTE: In a real decoder, additional saved state lives here (sizes, MV state, etc.).
};

struct Dpb {
    DpbSlot slot[8];
};

// ---- Utility: wrap-aware relative distance from the spec ----
static inline int get_relative_dist(int a, int b, uint8_t enable_order_hint, uint8_t order_hint_bits) {
    if (!enable_order_hint) return 0;
    const int m = 1 << (order_hint_bits - 1);
    int diff = a - b;
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

// ---- Sign-bias computation for the 7 ref frames ----
static inline void compute_ref_frame_sign_bias(
    uint8_t out_bias[7],
    const uint8_t ref_frame_idx[7],
    const Dpb& dpb,
    int curr_hint,
    const Av1Seq& seq
){
    for (int i = 0; i < 7; ++i) {
        const int hint = dpb.slot[ ref_frame_idx[i] ].order_hint;
        out_bias[i] = seq.enable_order_hint ? (get_relative_dist(hint, curr_hint, 1, seq.order_hint_bits) > 0) : 0;
    }
}

// ---- Apply refresh_frame_flags to DPB (reference frame update process) ----
static inline void dpb_apply_refresh(Dpb& dpb, uint8_t refresh_frame_flags, int curr_hint) {
    for (int i = 0; i < 8; ++i) {
        if ((refresh_frame_flags >> i) & 1) {
            dpb.slot[i].valid = 1;
            dpb.slot[i].order_hint = curr_hint;
        }
    }
}

// ---- Handle error-resilient ref_order_hint[] signaling ----
static inline void dpb_apply_signaled_ref_order_hints(
    Dpb& dpb, const Av1FrameHdr& fh, const Av1Seq& seq)
{
    if (!seq.enable_order_hint) return;
    if (!fh.error_resilient_mode) return;
    if (!fh.has_ref_order_hint) return;

    for (int i = 0; i < 8; ++i) {
        if (!dpb.slot[i].valid) continue;
        if (dpb.slot[i].order_hint != fh.ref_order_hint[i]) {
            // Spec: if the signaled hint differs, this slot becomes invalid
            dpb.slot[i].valid = 0;
        }
    }
}
```

---

## 1) Per‑AU Driver (syntax → DPB)

```cpp
struct PerFrameComputed {
    int      curr_order_hint;
    uint8_t  ref_frame_sign_bias[7];
    uint8_t  ref_frame_idx[7]; // final mapping after explicit or short-signaling resolution
};

// Placeholder for short-signaling resolver (derive 7 refs from {last,gold} + DPB per spec)
static inline void set_frame_refs_short_signaling(uint8_t out_idx[7],
                                                  uint8_t last_idx, uint8_t gold_idx,
                                                  const Dpb& dpb, const Av1Seq& seq, int curr_hint)
{
    // Pseudo: AOM's set_frame_refs chooses nearest forward for LAST-like,
    // nearest backward for GOLD/ALT, etc. For this walkthrough, just seed with
    // last_idx and gold_idx and fill others monotonically.
    out_idx[0] = last_idx;  // LAST
    out_idx[3] = gold_idx;  // GOLDEN
    // Fill the rest with any valid slots (implementation-defined here).
    int w = 0;
    for (int s = 0; s < 8; ++s) {
        if (s == last_idx || s == gold_idx) continue;
        if (!dpb.slot[s].valid) continue;
        for (; w < 7; ++w) if (w != 0 && w != 3 && out_idx[w] == 0xFF) break;
        if (w < 7) out_idx[w] = s;
    }
    // Any still unfilled -> pick a valid slot or 0.
    for (int i = 0; i < 7; ++i) if (out_idx[i] == 0xFF) out_idx[i] = last_idx;
}

// Build final ref_frame_idx[7] for this frame
static inline void build_ref_frame_idx(uint8_t out_idx[7], const Av1FrameHdr& fh,
                                       const Dpb& dpb, const Av1Seq& seq, int curr_hint)
{
    for (int i = 0; i < 7; ++i) out_idx[i] = 0xFF;
    if (fh.refs.frame_refs_short_signaling) {
        set_frame_refs_short_signaling(out_idx, fh.refs.last_frame_idx, fh.refs.gold_frame_idx,
                                       dpb, seq, curr_hint);
    } else {
        for (int i = 0; i < 7; ++i) out_idx[i] = fh.refs.ref_frame_idx[i];
    }
}

// Main per-frame processing (decode-order)
static inline void process_au(const Av1Seq& seq, const Av1FrameHdr& fh, Dpb& dpb,
                              PerFrameComputed* out)
{
    // 1) Possibly apply signaled ref_order_hint[] (error-resilient)
    dpb_apply_signaled_ref_order_hints(dpb, fh, seq);

    // 2) Resolve current OrderHint
    const int curr_hint = fh.order_hint & ((1 << seq.order_hint_bits) - 1);
    out->curr_order_hint = curr_hint;

    // 3) Handle show_existing_frame fast path: load state from DPB slot
    if (fh.show_existing_frame) {
        const int idx = fh.frame_to_show_map_idx;
        // Spec §7.21: load from slot[idx] into "current frame variables".
        // (For this walkthrough we don't mutate here; update still follows.)
    }

    // 4) Build the reference list indices
    build_ref_frame_idx(out->ref_frame_idx, fh, dpb, seq, curr_hint);

    // 5) Compute sign-bias
    compute_ref_frame_sign_bias(out->ref_frame_sign_bias, out->ref_frame_idx, dpb, curr_hint, seq);

    // 6) [Decode/filters happen outside DPB manager]

    // 7) Reference frame update (apply refresh flags)
    uint8_t rff = fh.refresh_frame_flags;
    // Key+show or SWITCH can force full refresh; caller should set rff accordingly
    dpb_apply_refresh(dpb, rff, curr_hint);
}
```

---

## 2) The 8‑Frame Walkthrough with Calls

The following code creates a small test harness that mirrors the earlier 8‑frame scenario.
It shows **exact calls**, **inputs**, and **DPB mutations** per frame.

```cpp
// Small helper for initializing DPB
static inline void dpb_init(Dpb& dpb) {
    for (int i = 0; i < 8; ++i) {
        dpb.slot[i].valid = 0;
        dpb.slot[i].slot_id = i;
        dpb.slot[i].order_hint = 0;
    }
}

// Print (for debugging / notes)
static inline void dump_dpb(const Dpb& dpb) {
    // (Replace with real logging in a library.)
    // Example: printf("S%d: V=%d OH=%d\n", i, dpb.slot[i].valid, dpb.slot[i].order_hint);
}

// Fill a frame header quickly
static inline Av1FrameHdr mk_fh(Av1FrameType t, int show, int show_exist,
                                int to_show_idx, int oh, uint8_t rff,
                                const Av1Refs& r, uint8_t err_res=0,
                                uint8_t has_roh=0, const uint8_t* roh=nullptr)
{
    Av1FrameHdr fh{};
    fh.frame_type = t;
    fh.show_frame = (uint8_t)show;
    fh.show_existing_frame = (uint8_t)show_exist;
    fh.frame_to_show_map_idx = (uint8_t)to_show_idx;
    fh.order_hint = (uint8_t)oh;
    fh.refresh_frame_flags = rff;
    fh.refs = r;
    fh.error_resilient_mode = err_res;
    fh.has_ref_order_hint = has_roh;
    if (has_roh && roh) for (int i=0;i<8;++i) fh.ref_order_hint[i]=roh[i];
    return fh;
}

void walkthrough_example() {
    Av1Seq seq{1, 3}; // enable_order_hint=1, order_hint_bits=3 (wrap at 8)
    Dpb dpb; dpb_init(dpb);
    PerFrameComputed out{};

    // Step 0: initial DPB (all invalid)

    // --- Frame 0: KEY + show, OH=0, refresh all ---
    Av1Refs r0{}; r0.frame_refs_short_signaling = 0; // irrelevant for key
    Av1FrameHdr fh0 = mk_fh(KEY_FRAME, 1, 0, 0, /*OH*/0,
                            /*rff*/0xFF, r0);
    // Force full refresh for key+show
    fh0.refresh_frame_flags = 0xFF;
    process_au(seq, fh0, dpb, &out);
    // Result: slots 0..7 valid, OH=0

    // --- Frame 1: INTER show, OH=1, rff=bit0 ---
    Av1Refs r1{}; r1.frame_refs_short_signaling = 0;
    // ref_frame_idx[7] → [0,1,2,3,4,5,6] (all OH=0 at this moment)
    for (int i=0;i<7;++i) r1.ref_frame_idx[i] = i;
    Av1FrameHdr fh1 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/1,
                            /*rff*/0b00000001, r1);
    process_au(seq, fh1, dpp=dpb, &out);
    // After: slot0 -> OH=1; slots1..7 remain OH=0
    // Sign-bias for all refs: forward (bias=0)

    // --- Frame 2: INTER show, OH=2, rff=bit1 ---
    Av1Refs r2{}; r2.frame_refs_short_signaling = 0;
    // Choose LAST=slot0 (index 0 now has OH=1)
    for (int i=0;i<7;++i) r2.ref_frame_idx[i] = i; // keep simple
    Av1FrameHdr fh2 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/2,
                            /*rff*/0b00000010, r2);
    process_au(seq, fh2, dpb, &out);
    // After: slot1 -> OH=2; slot0=OH=1; others=OH=0

    // --- Frame 3: INTER show, OH=3, rff=bit2 ---
    Av1Refs r3{}; r3.frame_refs_short_signaling = 0;
    // e.g., LAST=slot1 (OH=2), GOLD=slot0 (OH=1)
    for (int i=0;i<7;++i) r3.ref_frame_idx[i] = i;
    Av1FrameHdr fh3 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/3,
                            /*rff*/0b00000100, r3);
    process_au(seq, fh3, dpb, &out);
    // After: slot2 -> OH=3

    // --- Frame 4: INTER no-show, OH=7, rff=bit7 (ALTREF store) ---
    Av1Refs r4{}; r4.frame_refs_short_signaling = 0;
    for (int i=0;i<7;++i) r4.ref_frame_idx[i] = i;
    Av1FrameHdr fh4 = mk_fh(INTER_FRAME, 0, 0, 0, /*OH*/7,
                            /*rff*/0b10000000, r4);
    process_au(seq, fh4, dpb, &out);
    // After: slot7 -> OH=7 (backward ref for future frames)

    // --- Frame 5: INTER show, OH=4, rff=bit3 ---
    Av1Refs r5{}; r5.frame_refs_short_signaling = 0;
    // Ensure ALTREF uses slot7 and LAST uses slot2
    r5.ref_frame_idx[0] = 2; // LAST = slot2 (OH=3)
    r5.ref_frame_idx[6] = 7; // ALTREF = slot7 (OH=7)
    // Fill remaining indices (keep prior defaults)
    for (int i=1;i<6;++i) if (i!=6) r5.ref_frame_idx[i] = i;
    Av1FrameHdr fh5 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/4,
                            /*rff*/0b00001000, r5);
    process_au(seq, fh5, dpb, &out);
    // After: slot3 -> OH=4
    // Bias: LAST forward (0), ALTREF backward (1)

    // --- Frame 6: INTER show, OH=5, rff=bit4 ---
    Av1Refs r6{}; r6.frame_refs_short_signaling = 0;
    r6.ref_frame_idx[0] = 3; // LAST = slot3 (OH=4)
    r6.ref_frame_idx[6] = 7; // ALTREF = slot7 (OH=7)
    for (int i=1;i<6;++i) if (i!=6) r6.ref_frame_idx[i] = i;
    Av1FrameHdr fh6 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/5,
                            /*rff*/0b00010000, r6);
    process_au(seq, fh6, dpb, &out);
    // After: slot4 -> OH=5

    // --- Frame 7: INTER show, OH=6, rff=bit5 ---
    Av1Refs r7{}; r7.frame_refs_short_signaling = 0;
    r7.ref_frame_idx[0] = 4; // LAST = slot4 (OH=5)
    r7.ref_frame_idx[6] = 7; // ALTREF = slot7 (OH=7)
    for (int i=1;i<6;++i) if (i!=6) r7.ref_frame_idx[i] = i;
    Av1FrameHdr fh7 = mk_fh(INTER_FRAME, 1, 0, 0, /*OH*/6,
                            /*rff*/0b00100000, r7);
    process_au(seq, fh7, dpb, &out);
    // After: slot5 -> OH=6, slot7 remains OH=7 (can be shown later via show_existing_frame)
}
```

> **Note:** In a production decoder, §7.20/§7.21 save/restore many more fields than `order_hint`.
This example focuses on sign‑bias and `RefOrderHint[]` because those are the mechanisms directly needed for
**ref frame mapping** and **forward/backward** classification.

---

## 3) Minimal Unit‑Style Checks (optional)

```cpp
static inline void expect_eq_int(const char* what, int a, int b) {
    // Replace with real test infra; here just document intent.
    // assert(a == b);
}

void walkthrough_asserts(const Dpb& dpb) {
    expect_eq_int("slot7.valid", dpb.slot[7].valid, 1);
    expect_eq_int("slot7.OH",    dpb.slot[7].order_hint, 7);
    // Add checks for slot0..5 after each frame if desired.
}
```
