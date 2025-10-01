#include "dpb.hpp"

// ---- Utility implementation ----
int get_relative_dist(int a, int b, uint8_t enable_order_hint, uint8_t order_hint_bits) {
    if (!enable_order_hint) return 0;
    const int m = 1 << (order_hint_bits - 1);
    int diff = a - b;
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

void compute_ref_frame_sign_bias(uint8_t out_bias[7],
                                 const uint8_t ref_frame_idx[7],
                                 const Dpb& dpb,
                                 int curr_hint,
                                 const Av1Seq& seq) {
    for (int i = 0; i < 7; ++i) {
        const int hint = dpb.slot[ ref_frame_idx[i] ].order_hint;
        out_bias[i] = seq.enable_order_hint
            ? (get_relative_dist(hint, curr_hint, 1, seq.order_hint_bits) > 0)
            : 0;
    }
}

void dpb_apply_refresh(Dpb& dpb, uint8_t refresh_frame_flags, int curr_hint) {
    for (int i = 0; i < 8; ++i) {
        if ((refresh_frame_flags >> i) & 1) {
            dpb.slot[i].valid = 1;
            dpb.slot[i].order_hint = curr_hint;
        }
    }
}

void dpb_apply_signaled_ref_order_hints(Dpb& dpb, const Av1FrameHdr& fh, const Av1Seq& seq) {
    if (!seq.enable_order_hint) return;
    if (!fh.error_resilient_mode) return;
    if (!fh.has_ref_order_hint) return;
    for (int i = 0; i < 8; ++i) {
        if (!dpb.slot[i].valid) continue;
        if (dpb.slot[i].order_hint != fh.ref_order_hint[i]) {
            dpb.slot[i].valid = 0;
        }
    }
}

// --- Reference index builder (simplified short signaling) ---
static inline void set_frame_refs_short_signaling(uint8_t out_idx[7],
                                                  uint8_t last_idx, uint8_t gold_idx,
                                                  const Dpb& dpb, const Av1Seq& seq, int curr_hint) {
    out_idx[0] = last_idx;
    out_idx[3] = gold_idx;
    for (int i = 0; i < 7; ++i) if (out_idx[i] == 0xFF) out_idx[i] = last_idx;
}

void build_ref_frame_idx(uint8_t out_idx[7], const Av1FrameHdr& fh,
                         const Dpb& dpb, const Av1Seq& seq, int curr_hint) {
    for (int i = 0; i < 7; ++i) out_idx[i] = 0xFF;
    if (fh.refs.frame_refs_short_signaling) {
        set_frame_refs_short_signaling(out_idx, fh.refs.last_frame_idx, fh.refs.gold_frame_idx,
                                       dpb, seq, curr_hint);
    } else {
        for (int i = 0; i < 7; ++i) out_idx[i] = fh.refs.ref_frame_idx[i];
    }
}

void process_au(const Av1Seq& seq, const Av1FrameHdr& fh, Dpb& dpb,
                PerFrameComputed* out) {
    dpb_apply_signaled_ref_order_hints(dpb, fh, seq);

    const int curr_hint = fh.order_hint & ((1 << seq.order_hint_bits) - 1);
    out->curr_order_hint = curr_hint;

    if (fh.show_existing_frame) {
        const int idx = fh.frame_to_show_map_idx;
        (void)idx; // Spec §7.21: load from slot[idx]
    }

    build_ref_frame_idx(out->ref_frame_idx, fh, dpb, seq, curr_hint);

    compute_ref_frame_sign_bias(out->ref_frame_sign_bias,
                                out->ref_frame_idx, dpb,
                                curr_hint, seq);

    dpb_apply_refresh(dpb, fh.refresh_frame_flags, curr_hint);
}

void dpb_init(Dpb& dpb) {
    for (int i = 0; i < 8; ++i) {
        dpb.slot[i].valid = 0;
        dpb.slot[i].slot_id = i;
        dpb.slot[i].order_hint = 0;
    }
}

// --- Example Walkthrough ---
static Av1FrameHdr mk_fh(Av1FrameType t, int show, int show_exist,
                         int to_show_idx, int oh, uint8_t rff, const Av1Refs& r) {
    Av1FrameHdr fh{};
    fh.frame_type = t;
    fh.show_frame = (uint8_t)show;
    fh.show_existing_frame = (uint8_t)show_exist;
    fh.frame_to_show_map_idx = (uint8_t)to_show_idx;
    fh.order_hint = (uint8_t)oh;
    fh.refresh_frame_flags = rff;
    fh.refs = r;
    return fh;
}

void walkthrough_example() {
    Av1Seq seq{1, 3}; // enable_order_hint=1, order_hint_bits=3
    Dpb dpb; dpb_init(dpb);
    PerFrameComputed out{};

    // Frame 0: KEY + show, OH=0, refresh all
    Av1Refs r0{};
    Av1FrameHdr fh0 = mk_fh(KEY_FRAME, 1, 0, 0, 0, 0xFF, r0);
    process_au(seq, fh0, dpb, &out);

    // Frame 1: INTER show, OH=1, refresh slot0
    Av1Refs r1{}; for (int i=0;i<7;++i) r1.ref_frame_idx[i] = i;
    Av1FrameHdr fh1 = mk_fh(INTER_FRAME, 1, 0, 0, 1, 0b00000001, r1);
    process_au(seq, fh1, dpb, &out);

    // Frame 2: INTER show, OH=2, refresh slot1
    Av1Refs r2{}; for (int i=0;i<7;++i) r2.ref_frame_idx[i] = i;
    Av1FrameHdr fh2 = mk_fh(INTER_FRAME, 1, 0, 0, 2, 0b00000010, r2);
    process_au(seq, fh2, dpb, &out);

    // Frame 3: INTER show, OH=3, refresh slot2
    Av1Refs r3{}; for (int i=0;i<7;++i) r3.ref_frame_idx[i] = i;
    Av1FrameHdr fh3 = mk_fh(INTER_FRAME, 1, 0, 0, 3, 0b00000100, r3);
    process_au(seq, fh3, dpb, &out);

    // Frame 4: INTER no-show, OH=7, refresh slot7
    Av1Refs r4{}; for (int i=0;i<7;++i) r4.ref_frame_idx[i] = i;
    Av1FrameHdr fh4 = mk_fh(INTER_FRAME, 0, 0, 0, 7, 0b10000000, r4);
    process_au(seq, fh4, dpb, &out);

    // Frame 5: INTER show, OH=4, refresh slot3
    Av1Refs r5{}; r5.ref_frame_idx[0]=2; r5.ref_frame_idx[6]=7;
    Av1FrameHdr fh5 = mk_fh(INTER_FRAME, 1, 0, 0, 4, 0b00001000, r5);
    process_au(seq, fh5, dpb, &out);

    // Frame 6: INTER show, OH=5, refresh slot4
    Av1Refs r6{}; r6.ref_frame_idx[0]=3; r6.ref_frame_idx[6]=7;
    Av1FrameHdr fh6 = mk_fh(INTER_FRAME, 1, 0, 0, 5, 0b00010000, r6);
    process_au(seq, fh6, dpb, &out);

    // Frame 7: INTER show, OH=6, refresh slot5
    Av1Refs r7{}; r7.ref_frame_idx[0]=4; r7.ref_frame_idx[6]=7;
    Av1FrameHdr fh7 = mk_fh(INTER_FRAME, 1, 0, 0, 6, 0b00100000, r7);
    process_au(seq, fh7, dpb, &out);
}