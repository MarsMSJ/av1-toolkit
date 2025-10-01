# DPB Management 

Pseudo-C++ 

---

## dpb.hpp

```cpp
#pragma once
#include <cstdint>

enum Av1FrameType : uint8_t { KEY_FRAME, INTER_FRAME, INTRA_ONLY_FRAME, SWITCH_FRAME };

struct Av1Seq {
    uint8_t enable_order_hint;
    uint8_t order_hint_bits;
};

struct Av1Refs {
    uint8_t frame_refs_short_signaling;
    uint8_t last_frame_idx;
    uint8_t gold_frame_idx;
    uint8_t ref_frame_idx[7];
};

struct Av1FrameHdr {
    Av1FrameType frame_type;
    uint8_t show_frame;
    uint8_t show_existing_frame;
    uint8_t frame_to_show_map_idx;
    uint8_t refresh_frame_flags;
    uint8_t error_resilient_mode;
    uint8_t order_hint;
    Av1Refs refs;
    uint8_t has_ref_order_hint;
    uint8_t ref_order_hint[8];
};

struct DpbSlot {
    uint8_t valid;
    uint8_t slot_id;
    int     order_hint;
};

struct Dpb {
    DpbSlot slot[8];
};

struct PerFrameComputed {
    int      curr_order_hint;
    uint8_t  ref_frame_sign_bias[7];
    uint8_t  ref_frame_idx[7];
};

// ---- Utility functions ----
int get_relative_dist(int a, int b, uint8_t enable_order_hint, uint8_t order_hint_bits);

void compute_ref_frame_sign_bias(uint8_t out_bias[7],
                                 const uint8_t ref_frame_idx[7],
                                 const Dpb& dpb,
                                 int curr_hint,
                                 const Av1Seq& seq);

void dpb_apply_refresh(Dpb& dpb, uint8_t refresh_frame_flags, int curr_hint);

void dpb_apply_signaled_ref_order_hints(Dpb& dpb, const Av1FrameHdr& fh, const Av1Seq& seq);

void build_ref_frame_idx(uint8_t out_idx[7], const Av1FrameHdr& fh,
                         const Dpb& dpb, const Av1Seq& seq, int curr_hint);

void process_au(const Av1Seq& seq, const Av1FrameHdr& fh, Dpb& dpb,
                PerFrameComputed* out);

void dpb_init(Dpb& dpb);