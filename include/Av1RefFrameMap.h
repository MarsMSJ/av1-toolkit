//
// Created by Mario M Sarria Jr on 10/9/25.
//

#ifndef AV1_REF_FRAME_MAP_H
#define AV1_REF_FRAME_MAP_H
#include <array>


class Av1RefFrameMap
{

};

#pragma once
#include <cstdint>
#include <array>

// --- Constants matching AOM semantics ---
#ifndef NUM_REF_FRAMES
#define NUM_REF_FRAMES 8
#endif

// AOM ref frame types used inside a coded frame.
// (Only needed if you want sign bias per LAST..ALTREF2; else you can skip.)
enum class RefFrameType : int
{
    INTRA_FRAME = 0,
    LAST_FRAME = 1,
    LAST2_FRAME = 2,
    LAST3_FRAME = 3,
    GOLDEN_FRAME = 4,
    BWDREF_FRAME = 5,
    ALTREF2_FRAME = 6,
    ALTREF_FRAME = 7,
    REFS_PER_FRAME = 7  // number of inter refs signaled for a frame
};

struct Av1RefMapMgr {
    // Sentinel used for “no buffer”
    static constexpr uint8_t kInvalidFbIdx = 0x7F;

    // Slot -> buffer index (frame buffer pool index)
    std::array<uint8_t, NUM_REF_FRAMES> ref_frame_map;
    // Slot -> order_hint for the frame currently stored in that slot
    std::array<int16_t, NUM_REF_FRAMES> ref_frame_offset;

    // AOM order hint context (copied from sequence header bits)
    struct OrderHintCtx {
        uint8_t enable_order_hint; // seqhdr.enable_order_hint
        uint8_t order_hint_bits;   // seqhdr.order_hint_bits_minus_1 + 1 (0 if disabled)

        // Compute wrapped relative distance: a - b (mod 2^bits), returned in signed range.
        int get_relative_dist(int a, int b) const {
            if (!enable_order_hint || order_hint_bits == 0) return 0;
            const int bits = order_hint_bits;
            const int m = 1 << bits;
            const int diff = (a - b) & (m - 1);
            const int m2 = m >> 1;
            return (diff > m2) ? diff - m : diff;
        }
    };

    // Extract of what we need from the *frame-level* headers
    struct FrameCtx {
        // From uncompressed / frame headers
        uint8_t frame_type;            // KEY(0), INTRA_ONLY(1), INTER(2), SWITCH(3)  -- AOM numeric order
        uint8_t show_frame;            // 1 if current coded frame is displayed
        uint8_t show_existing_frame;   // 1 if this TU only displays an existing ref
        uint8_t refresh_frame_flags;   // 8-bit mask; bit i refreshes slot i with current frame

        // Order hint for current frame (if enable_order_hint)
        int16_t order_hint;            // frame_hdr.order_hint (wraps to seq.order_hint_bits)
        // For computing sign bias per ref type (optional convenience):
        // The 7 ref frame types map to reference slots via ref_frame_idx[i] computed earlier.
        // If you don’t care about per-type bias, you can omit these.
        uint8_t ref_frame_idx[REFS_PER_FRAME]; // each entry is a slot index 0..7
    };

    // --- Lifecycle ---

    void init() {
        ref_frame_map.fill(kInvalidFbIdx);
        ref_frame_offset.fill(0);
    }

    // Call after decoding a frame *that actually produced new pixels*.
    // If (show_existing_frame==1), this function will early-out and not mutate the map.
    //
    // - cur_fb_idx: buffer-pool index of the newly decoded frame
    // - seq_oh:     from sequence header (enable_order_hint, order_hint_bits)
    // - frm:        from frame/uncompressed headers
    void update_after_decode(uint8_t cur_fb_idx,
                             const OrderHintCtx& seq_oh,
                             const FrameCtx& frm) {
        // “show_existing_frame” does not decode a new frame; do not touch the map.
        if (frm.show_existing_frame) return;

        // For KEY+shown or SWITCH, spec/libaom behavior is to refresh all slots.
        uint8_t mask = frm.refresh_frame_flags;
        const bool key_shown = (frm.frame_type == 0 /*KEY*/) && (frm.show_frame != 0);
        const bool is_switch = (frm.frame_type == 3 /*SWITCH*/);
        if (key_shown || is_switch) mask = 0xFF;

        // Write current decoded frame into all selected slots, and stamp its order_hint
        for (int i = 0; i < NUM_REF_FRAMES; ++i) {
            if (mask & (1u << i)) {
                ref_frame_map[i]    = cur_fb_idx;
                ref_frame_offset[i] = frm.order_hint;  // store the display order of this frame for the slot
            }
        }
    }

    // Optional: compute per-reference sign bias for the 7 ref types used by this frame.
    // (Sign bias is “is this ref forward (positive) or backward (negative) vs current frame?”)
    //
    // out_sign_bias[i] = 1 if (ref is backward in display order wrt current), 0 otherwise.
    // This is consistent with libaom’s usage where backward refs (ALT/ALT2/BWD) tend to be “future” frames.
    void compute_ref_sign_bias(const OrderHintCtx& seq_oh,
                               const FrameCtx& frm,
                               uint8_t out_sign_bias[REFS_PER_FRAME]) const
    {
        for (int i = 0; i < REFS_PER_FRAME; ++i) {
            const uint8_t slot = frm.ref_frame_idx[i]; // slot 0..7 selected for this ref type
            const int16_t ref_oh = ref_frame_offset[slot];
            const int rel = seq_oh.get_relative_dist(ref_oh, frm.order_hint);
            // If ref is *ahead* of current in display order (positive rel),
            // then relative to the current frame it’s a “forward” reference.
            // AOM’s “sign bias” is often encoded as (rel < 0) to mark backward refs; pick what your MC uses.
            out_sign_bias[i] = (rel < 0) ? 1 : 0;
        }
    }

    // Utility: reset everything (e.g., new sequence, error resilient reset)
    void invalidate_all() {
        for (int i = 0; i < NUM_REF_FRAMES; ++i) {
            ref_frame_map[i]    = kInvalidFbIdx;
            ref_frame_offset[i] = 0;
        }
    }
};

#pragma once
#include <cstdint>
#include <array>

#ifndef NUM_REF_FRAMES
#define NUM_REF_FRAMES 8
#endif

// AOM ref types in signal order (7 inter refs):
enum { LAST_FRAME=1, LAST2_FRAME, LAST3_FRAME, GOLDEN_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME };
static constexpr int REFS_PER_FRAME = 7;

struct RefMap {
    static constexpr uint8_t kInvalidFbIdx = 0x7F;

    // DPB slots → fb index + order_hint
    std::array<uint8_t,  NUM_REF_FRAMES> ref_frame_map{};
    std::array<int16_t,  NUM_REF_FRAMES> ref_frame_offset{};

    void init() {
        ref_frame_map.fill(kInvalidFbIdx);
        ref_frame_offset.fill(0);
    }

    struct OrderHintCtx {
        uint8_t enable_order_hint;  // seq.enable_order_hint
        uint8_t order_hint_bits;    // 0 if disabled, else number of bits
        int get_relative_dist(int a, int b) const {
            if (!enable_order_hint || order_hint_bits == 0) return 0;
            const int m = 1 << order_hint_bits, m2 = m >> 1;
            const int diff = (a - b) & (m - 1);
            return (diff > m2) ? diff - m : diff;
        }
    };

    struct FrameCtx {
        uint8_t frame_type;            // KEY=0, INTRA_ONLY=1, INTER=2, SWITCH=3  (AOM numbering)
        uint8_t show_frame;            // 0/1
        uint8_t show_existing_frame;   // 0/1
        uint8_t refresh_frame_flags;   // 8-bit mask, bit i refreshes slot i
        int16_t order_hint;            // current frame OH (if enabled)

        // From dav1d “Set frame refs”:
        // ref_frames[i] = slot index [0..7] chosen for LAST..ALTREF in that order.
        uint8_t ref_frames[REFS_PER_FRAME];

        // From dav1d: per-ref-type sign bias (same order as ref_frames above).
        // Convention used here: 1 = backward (ref is earlier in display order), 0 = forward/same
        uint8_t ref_frame_sign_bias[REFS_PER_FRAME];
    };

    // 1) Update slots after decoding a new frame
    void update_after_decode(uint8_t cur_fb_idx, const OrderHintCtx& oh, const FrameCtx& f) {
        if (f.show_existing_frame) return;  // nothing decoded → no DPB writes

        uint8_t mask = f.refresh_frame_flags;
        const bool key_shown = (f.frame_type == 0) && (f.show_frame != 0);
        const bool is_switch = (f.frame_type == 3);
        if (key_shown || is_switch) mask = 0xFF; // refresh all per spec/libaom

        for (int i = 0; i < NUM_REF_FRAMES; ++i) {
            if (mask & (1u << i)) {
                ref_frame_map[i]    = cur_fb_idx;
                ref_frame_offset[i] = f.order_hint;
            }
        }
    }

    // 2) Build a per-slot sign-bias array from dav1d’s per-ref-type bias and mapping.
    // Any slot not referenced by the current frame keeps its previous bias (or 0 if unknown).
    // out_slot_bias[i] = 1 if that slot is “backward” vs current frame, else 0.
    void derive_slot_sign_bias_from_dav1d(const FrameCtx& f,
                                          uint8_t out_slot_bias[NUM_REF_FRAMES]) const
    {
        // Start neutral
        for (int s = 0; s < NUM_REF_FRAMES; ++s) out_slot_bias[s] = 0;

        // Map each ref type’s bias onto its chosen slot this frame
        for (int r = 0; r < REFS_PER_FRAME; ++r) {
            const uint8_t slot = f.ref_frames[r];      // slot 0..7
            const uint8_t bias = f.ref_frame_sign_bias[r] ? 1 : 0;
            out_slot_bias[slot] = bias;
        }
    }

    // 3) (Optional) If you prefer computing bias from order hints instead of passing dav1d’s bias:
    void compute_slot_sign_bias_from_offsets(const OrderHintCtx& oh, const FrameCtx& f,
                                             uint8_t out_slot_bias[NUM_REF_FRAMES]) const
    {
        for (int s = 0; s < NUM_REF_FRAMES; ++s) {
            const int rel = oh.get_relative_dist(ref_frame_offset[s], f.order_hint);
            out_slot_bias[s] = (rel < 0) ? 1 : 0;  // 1 = backward
        }
    }
};

// Build contexts once per frame
RefMap::OrderHintCtx oh {
    .enable_order_hint = (uint8_t)seq.enable_order_hint,
    .order_hint_bits   = (uint8_t)(seq.enable_order_hint ? seq.order_hint_n_bits : 0),
};

RefMap::FrameCtx frm {
    .frame_type            = (uint8_t)fh.frame_type,
    .show_frame            = (uint8_t)fh.show_frame,
    .show_existing_frame   = (uint8_t)fh.show_existing_frame,
    .refresh_frame_flags   = (uint8_t)fh.refresh_frame_flags,
    .order_hint            = (int16_t)fh.order_hint,
};

// Fill from dav1d’s “Set frame refs” results:
for (int i = 0; i < REFS_PER_FRAME; ++i) {
    frm.ref_frames[i]          = dav1d_ref_slots[i];      // 0..7
    frm.ref_frame_sign_bias[i] = dav1d_ref_sign_bias[i];  // 0/1 as provided by dav1d
}

// After reconstructing pixels (not for show_existing_frame):
refmap.update_after_decode(cur_fb_idx, oh, frm);

// If you need per-slot bias for motion comp, either:
uint8_t slot_bias[NUM_REF_FRAMES];
refmap.derive_slot_sign_bias_from_dav1d(frm, slot_bias);
// …or compute from order hints (works even if dav1d didn't expose bias):
refmap.compute_slot_sign_bias_from_offsets(oh, frm, slot_bias);


// No exceptions; header-only style utility.
#include <cstdint>
#include <array>
#include <algorithm>

namespace av1 {

// AV1 constants
constexpr int NUM_REF_FRAMES  = 8;   // reference slots
constexpr int REFS_PER_FRAME  = 7;   // LAST..ALTREF (LAST, LAST2, LAST3, GOLDEN, BWDREF, ALTREF2, ALTREF)

// Helper: spec-accurate get_relative_dist(a, b)
// Spec §5.9.3: diff = (a - b) sign-extended over OrderHintBits
static inline int get_relative_dist(uint32_t a, uint32_t b,
                                    int order_hint_bits,
                                    bool enable_order_hint)
{
    if (!enable_order_hint) return 0;
    // OrderHintBits in [0..8] typically; guard
    if (order_hint_bits <= 0) return 0;
    const int m = 1 << (order_hint_bits - 1);
    int diff = static_cast<int>(a) - static_cast<int>(b);
    // Two’s complement sign-extend over OrderHintBits:
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

// Output container for “offsets” (relative distances) and sign bias.
struct RefOffsets {
    std::array<int, REFS_PER_FRAME> offset{};   // get_relative_dist(ref, cur)
    std::array<uint8_t, REFS_PER_FRAME> sign_bias{}; // 0 = forward, 1 = backward (spec RefFrameSignBias)
};

// Compute and cache per-ref temporal offsets for the current frame.
// Inputs:
//   cur_order_hint      - OrderHint for current frame (LSBs, per SequenceHdr)
//   ref_order_hint[8]   - OrderHint for each reference slot (LSBs)
//   ref_frame_idx[7]    - Indices into the 8 slots for LAST..ALTREF chosen by set_frame_refs()
//   order_hint_bits     - OrderHintBits from sequence header
//   enable_order_hint   - enable_order_hint flag from sequence header
//
// Behavior:
//   offset[i]    = get_relative_dist(ref_hint, cur_hint)
//   sign_bias[i] = 0 if forward (ref is expected to output before current), else 1 if backward
//                  i.e., bias = (offset[i] > 0)
static inline void UpdateRefOffsets(uint32_t cur_order_hint,
                                    const std::array<uint32_t, NUM_REF_FRAMES>& ref_order_hint,
                                    const std::array<int8_t,  REFS_PER_FRAME>&  ref_frame_idx,
                                    int order_hint_bits,
                                    bool enable_order_hint,
                                    RefOffsets& out)
{
    for (int i = 0; i < REFS_PER_FRAME; ++i) {
        const int8_t idx = ref_frame_idx[i];
        // If unset (-1), spec allows fill; here we zero it.
        if (idx < 0 || idx >= NUM_REF_FRAMES) {
            out.offset[i] = 0;
            out.sign_bias[i] = 0;
            continue;
        }

        const uint32_t ref_hint = ref_order_hint[static_cast<int>(idx)];
        const int d = get_relative_dist(ref_hint, cur_order_hint, order_hint_bits, enable_order_hint);
        out.offset[i] = d;

        // Spec: RefFrameSignBias = 0 for “forwards reference” (ref expected to output before current),
        // and 1 for “backwards reference”. Forward means ref earlier than current => distance < 0.
        // We store bias=1 iff ref is a backward reference (d > 0).
        out.sign_bias[i] = (d > 0) ? 1u : 0u;
    }
}

} // namespace av1
#endif //AV1_REF_FRAME_MAP_H