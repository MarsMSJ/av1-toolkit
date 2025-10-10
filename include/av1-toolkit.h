#pragma once
#include <iostream>

inline void say_hello() {
    std::cout << "Hello from header function!\n";
}

// av1_refs.h
// Minimal reference-frames scaffolding for an AV1 decoder.
// Focus: ref frame identifiers, ref map (8 slots), per-ref metadata, and order-hint math.
// This is a reference header to guide your own implementation.
//
// Notes:
// - Mirrors common AOM concepts: NUM_REF_FRAMES = 8 “slots”, REFS_PER_FRAME = 7 directional refs,
//   and the LAST/GOLDEN/BWDREF/ALTREF naming.
// - The map holds indices to your frame buffer pool (or 0x7F / -1 for invalid).
// - Order hints are unsigned ring values with order_hint_bits width; use get_relative_dist()
//   to compare them correctly (wrap-around aware).
// - No film grain surfaces here; reserve room in your DPB at 10 if you plan to add it later.

#ifndef AV1_REFS_H_
#define AV1_REFS_H_

#include <cstdint>
#include <cstddef>
#include <array>
#include <limits>

namespace av1 {

// -------------------------------------------------------------------------------------------------
// Constants & small utilities
// -------------------------------------------------------------------------------------------------

// AV1 has 8 "reference frame slots" (AKA reference frame map entries).
// Each slot points to a decoded frame buffer (or is invalid).
constexpr int AV1_NUM_REF_FRAMES   = 8;

// A coded frame can reference up to 7 directional reference types.
constexpr int AV1_REFS_PER_FRAME   = 7;

// Use -1 for "invalid/no frame buffer" (AOM uses -1 or 0xFF in places; pick what fits your code).
constexpr int AV1_INVALID_FB_IDX   = -1;

// Some code prefers 0x7F for “invalid slot” in an 8-bit map. Keep both handy if you interop with C.
constexpr uint8_t AV1_INVALID_SLOT_U8 = 0x7F;

// -------------------------------------------------------------------------------------------------
// Reference frame identifiers (AOM-aligned names)
// -------------------------------------------------------------------------------------------------
//
// INTRA_FRAME is 0 in the spec naming tables (no inter ref).
// Inter refs are [LAST, LAST2, LAST3, GOLDEN, BWDREF, ALTREF2, ALTREF].
//
// Many implementations store these as 1..7 for inter and 0 for INTRA. We expose both the
// “kind” (enumeration below) and arrays sized by AV1_REFS_PER_FRAME for the 7 inter refs.

enum class RefFrameKind : uint8_t {
  INTRA_FRAME  = 0,  // Not actually used as a directional ref entry
  LAST_FRAME   = 1,
  LAST2_FRAME  = 2,
  LAST3_FRAME  = 3,
  GOLDEN_FRAME = 4,
  BWDREF_FRAME = 5,
  ALTREF2_FRAME= 6,
  ALTREF_FRAME = 7
};

// Convenient iterable list of the 7 directional refs (excludes INTRA).
constexpr std::array<RefFrameKind, AV1_REFS_PER_FRAME> kInterRefOrder = {
  RefFrameKind::LAST_FRAME,
  RefFrameKind::LAST2_FRAME,
  RefFrameKind::LAST3_FRAME,
  RefFrameKind::GOLDEN_FRAME,
  RefFrameKind::BWDREF_FRAME,
  RefFrameKind::ALTREF2_FRAME,
  RefFrameKind::ALTREF_FRAME
};

// Some tools (e.g., motion vector sign) use a “sign bias” per ref type.
// Typical convention: 0 = forward (last/golden), 1 = backward (bwdref/alt).
// You compute/populate this per coded frame based on order hints (see below).
using RefSignBiasArray = std::array<uint8_t, AV1_NUM_REF_FRAMES>; // index by RefFrameKind casted to int

// -------------------------------------------------------------------------------------------------
// Order hint support
// -------------------------------------------------------------------------------------------------
//
// Order hints are modulo-2^order_hint_bits ring values.
// Enable/width lives in sequence header; per-frame order_hint is stored for ref selection & POC-ish math.

struct OrderHintInfo {
  uint8_t enable_order_hint = 0;  // 0/1
  uint8_t order_hint_bits   = 0;  // 0..8 in practice; 0 => disabled
};

// Wrap-aware distance between two order hints (a - b) in signed domain.
// Returns 0 if order hints are disabled (safe default).
static inline int get_relative_dist(const OrderHintInfo& oh, uint32_t a, uint32_t b) {
  if (!oh.enable_order_hint || oh.order_hint_bits == 0) return 0;
  const int bits = oh.order_hint_bits;
  const int m = 1 << bits;
  const int diff = static_cast<int>((a & (m - 1)) - (b & (m - 1)));
  const int m_half = m >> 1;
  // Normalize to signed range (−m/2, +m/2]
  return (diff >  m_half) ? (diff - m)
       : (diff <= -m_half) ? (diff + m)
                           : diff;
}

// Compare two order hints; true if a is “after” b in display order (wrap-aware).
static inline bool is_after(const OrderHintInfo& oh, uint32_t a, uint32_t b) {
  return get_relative_dist(oh, a, b) > 0;
}

// -------------------------------------------------------------------------------------------------
// Frame/Buffer metadata tied to a reference
// -------------------------------------------------------------------------------------------------

// Basic allocation/layout description for a decoded frame buffer.
// You can replace with your image class; keep a POD shim for DPB bookkeeping.
struct FramePlaneLayout {
  // Base pointers for Y, U, V (or single pointer if you use a packed surface).
  uint8_t* y  = nullptr;
  uint8_t* u  = nullptr;
  uint8_t* v  = nullptr;

  // Strides in bytes.
  int y_stride  = 0;
  int uv_stride = 0;

  // Bit depth (8/10/12).
  uint8_t bitdepth = 8;

  // Subsampling factors (0 or 1 typical for 4:2:0 => (1,1)).
  uint8_t subsampling_x = 1;
  uint8_t subsampling_y = 1;

  // Optional: format tag if you keep multiple surface flavors.
  uint8_t reserved_format = 0;
};

// Logical/visible frame size fields commonly used for ref checks & scaling.
struct FrameSize {
  uint16_t upscaled_width  = 0;  // upscaled_width in spec
  uint16_t frame_width     = 0;  // superres after-scaling width (coded frame)
  uint16_t frame_height    = 0;
  uint16_t render_width    = 0;  // display/render size from headers
  uint16_t render_height   = 0;
};

// Per-reference buffer metadata stored in the DPB/map entry.
// Keep this lightweight; heavy codec state should live elsewhere.
struct RefBufferMeta {
  uint32_t frame_id    = 0;   // 15/16-bit ID in spec; use 32-bit here for convenience
  uint32_t order_hint  = 0;   // ring value for ordering
  FrameSize size       {};    // frame & render sizes
  uint8_t   displayable= 1;   // 1 if can be shown (not a hidden show-existing edge case)
  uint8_t   intra_only = 0;   // was coded intra-only (useful for MV modes etc.)
  uint8_t   refreshed  = 0;   // set when this ref was written this frame
  uint8_t   ref_valid  = 0;   // slot validity guard
};

// A single “frame buffer” record (surface + meta). Your allocator owns lifetime.
// The DPB/map will store indices (or pointers) to these.
struct FrameBuffer {
  FramePlaneLayout planes {};
  RefBufferMeta    meta   {};
  // If you keep additional per-frame state shared by tools (gm params, CDEF/restoration state
  // copies, deblock strength maps, etc.), you can hang small identifiers/handles here.
  void*            user_tag = nullptr;
};

// -------------------------------------------------------------------------------------------------
// Reference frame map (8 slots) and directional references (7 entries) for a coded frame
// -------------------------------------------------------------------------------------------------

// Map 8 logical reference slots to your backing frame-buffer pool indices.
// Using int so AV1_INVALID_FB_IDX = -1 is easy to read in debuggers.
struct RefFrameMap {
  std::array<int, AV1_NUM_REF_FRAMES> slot_to_fb {}; // [slot] -> fb_idx, or -1 if invalid

  // Initialize all slots to invalid.
  void reset() {
    slot_to_fb.fill(AV1_INVALID_FB_IDX);
  }

  // Helper: return fb index for a ref kind (casts kind to slot: LAST=1..ALTREF=7).
  // Note: spec names map 1:1 from ref kind (1..7) to slots in many implementations.
  int fb_index_for(RefFrameKind kind) const {
    const int slot = static_cast<int>(kind); // 0==INTRA (not used); 1..7 are valid slots
    if (slot < 0 || slot >= AV1_NUM_REF_FRAMES) return AV1_INVALID_FB_IDX;
    return slot_to_fb[slot];
  }
};

// The set of 7 directional reference bindings selected for the current coded frame.
// Each entry holds a reference to a *slot* (or directly to a frame-buffer index if you prefer).
struct FrameRefs {
  // For each of the 7 directional refs (kInterRefOrder order), store which map slot it resolves to.
  // Example: ref_slot[0] corresponds to LAST_FRAME, ref_slot[6] to ALTREF_FRAME.
  std::array<int8_t, AV1_REFS_PER_FRAME> ref_slot {}; // -1 if not set

  // Optionally cache the resolved frame-buffer indices (after dereferencing the map).
  std::array<int, AV1_REFS_PER_FRAME>    ref_fb_idx {};

  // Motion sign bias per *ref kind* (index by (int)RefFrameKind).
  // Typically 0 for forward, 1 for backward; compute from order hints:
  //   bias[k] = (get_relative_dist(oh, cur_order_hint, ref_order_hint) < 0);
  RefSignBiasArray sign_bias {};

  void reset() {
    ref_slot.fill(static_cast<int8_t>(AV1_INVALID_FB_IDX));
    ref_fb_idx.fill(AV1_INVALID_FB_IDX);
    sign_bias.fill(0);
  }
};

// -------------------------------------------------------------------------------------------------
// DPB “directory” (lightweight index over your real surface pool)
// -------------------------------------------------------------------------------------------------
//
// This keeps just enough info to choose/validate refs and to update the map on refresh.
// The actual FrameBuffer pool can be a separate owner container (vector, ring, handle table, etc.).

struct DpbDirectoryEntry {
  int        fb_index = AV1_INVALID_FB_IDX; // index into your FrameBuffer pool
  uint32_t   order_hint = 0;
  uint8_t    valid = 0;
};

struct DpbDirectory {
  // Small ring or table you maintain per decoded frame to aid ref selection.
  // Size is not mandated by spec; keeping 8..16 is typical. This is *not* the 8-slot map;
  // it’s just a helper view for choosing best matches, pruning stale frames, etc.
  static constexpr int kMaxTracked = 16;

  std::array<DpbDirectoryEntry, kMaxTracked> entries {};
  int count = 0;

  void clear() { count = 0; }

  void push(int fb_index, uint32_t order_hint) {
    if (count >= kMaxTracked) return;
    entries[count++] = { fb_index, order_hint, 1 };
  }
};

// -------------------------------------------------------------------------------------------------
// Per-frame “reference context” (inputs you compute during parse to drive ref resolution)
// -------------------------------------------------------------------------------------------------

struct RefContext {
  OrderHintInfo order_hint_info {}; // from sequence header
  uint32_t      cur_order_hint = 0; // from uncompressed header / frame header

  // Parsed ref frame indices (frame_refs[] in spec) often come as slot IDs (0..7) or FB indices
  // depending on pipeline. Keep both channels so you can feed whichever your syntax parser gives.
  std::array<int8_t, AV1_REFS_PER_FRAME> parsed_ref_slots {}; // -1 if absent
  std::array<int,    AV1_REFS_PER_FRAME> parsed_ref_fb_idx {}; // -1 if absent

  // Convenience to set either channel uniformly.
  void reset() {
    cur_order_hint = 0;
    parsed_ref_slots.fill(static_cast<int8_t>(AV1_INVALID_FB_IDX));
    parsed_ref_fb_idx.fill(AV1_INVALID_FB_IDX);
  }
};

// -------------------------------------------------------------------------------------------------
// Refresh-mask and update instructions for the 8 slots (ref frame map update per decoded frame)
// -------------------------------------------------------------------------------------------------
//
// On a successful decode, certain slots are “refreshed” (overwritten) with the newly decoded frame.
// You’ll compute this mask from show_frame / show_existing_frame / refresh_frame_flags, etc.

struct MapUpdate {
  // Bit i set => refresh slot i with current frame (i in [0..7]).
  uint8_t refresh_slot_mask = 0;

  // If show_existing_frame, you may have a single source slot to display without decoding.
  // Keep it here for your higher level to act upon.
  int8_t show_existing_slot = -1;
};

// -------------------------------------------------------------------------------------------------
// Thin “view” into your backing storage so DPB/map logic can read metadata safely
// -------------------------------------------------------------------------------------------------

struct FrameStoreView {
  const FrameBuffer* (*get_fb)(int fb_index) = nullptr; // callback into your pool
};

// -------------------------------------------------------------------------------------------------
// Tiny helpers (non-owning, policy-free)
// -------------------------------------------------------------------------------------------------

// Resolve directional references to frame-buffer indices via the ref map.
static inline void resolve_ref_fb_indices(const RefFrameMap& map, FrameRefs& refs) {
  for (int i = 0; i < AV1_REFS_PER_FRAME; ++i) {
    const RefFrameKind kind = kInterRefOrder[i];
    const int fb = map.fb_index_for(kind);
    refs.ref_fb_idx[i] = fb;
  }
}

// Compute sign bias per ref kind using order hints (forward/backward classification).
static inline void compute_sign_bias(const OrderHintInfo& oh,
                                     uint32_t cur_oh,
                                     const FrameStoreView& store,
                                     const RefFrameMap& map,
                                     FrameRefs& refs) {
  // Default to forward (0).
  refs.sign_bias.fill(0);

  for (int i = 0; i < AV1_REFS_PER_FRAME; ++i) {
    const RefFrameKind kind = kInterRefOrder[i];
    const int fb = map.fb_index_for(kind);
    if (fb == AV1_INVALID_FB_IDX || !store.get_fb) continue;
    const FrameBuffer* fbp = store.get_fb(fb);
    if (!fbp || !fbp->meta.ref_valid) continue;

    const int rel = get_relative_dist(oh, fbp->meta.order_hint, cur_oh);
    // If reference is “after” current in display order, it’s backward => bias = 1.
    // We compare ref vs current, so rel > 0 means ref is newer/newer-display (backward ref).
    const int kind_index = static_cast<int>(kind);
    if (kind_index >= 0 && kind_index < AV1_NUM_REF_FRAMES) {
      refs.sign_bias[kind_index] = (rel > 0) ? 1u : 0u;
    }
  }
}

// ============================================================================
//  AV1 Per-Frame Decode Step (End-to-End Shim)
//  -------------------------------------------
//  This file provides a small, self-contained, *heavily commented* C++23
//  reference that stitches together three responsibilities:
//
//    1) Reference map / DPB management (8 slots)       → DPBManager
//    2) Reference offsets & sign bias (order hints)     → UpdateRefOffsets()
//    3) Per-frame decode control flow (AU step)         → DecodeOneAccessUnit()
//
//  Design goals:
//    - Exception-free, header-only style, no dynamic allocations here
//    - AOM/DAV1D-friendly naming & semantics
//    - Strong inline comments so this file can be read standalone as reference
//
//  What you must provide externally (integration points):
//    - A real frame decoder callback that takes parsed headers & ref mapping
//      and produces a decoded surface/buffer id for the current frame.
//    - A buffer pool (FrameBuf table) that your DPBManager can touch for
//      reference counting and order-hint bookkeeping.
//    - Real parsed Sequence/Frame headers from your syntax parser.
//
//  Notes:
//    - This file intentionally omits film grain, loop-restoration specifics,
//      error resilient corner cases, frame_ids, and multi-threading primitives.
//    - It focuses on the DPB and order-hint / sign-bias correctness so you can
//      drop this into your pipeline and iterate.
// ============================================================================

#include <cstdint>
#include <array>
#include <algorithm>

namespace av1 {

// ============================================================================
//  Constants / Small Utilities
// ============================================================================

constexpr int NUM_REF_FRAMES = 8;  // Fixed in AV1
constexpr int REFS_PER_FRAME = 7;  // LAST..ALTREF: LAST, LAST2, LAST3, GOLDEN, BWDREF, ALTREF2, ALTREF

// ----------------------------------------------------------------------------
// get_relative_dist()
// ----------------------------------------------------------------------------
// Spec-accurate modular distance over OrderHintBits (two's complement wrap).
// Returns signed distance from b -> a.
//   +ve => a is *after* b (backward reference relative to b)
//   -ve => a is *before* b (forward reference relative to b)
// When order hinting is disabled or OrderHintBits==0, returns 0.
// ----------------------------------------------------------------------------
static inline int get_relative_dist(uint32_t a, uint32_t b,
                                    int order_hint_bits,
                                    bool enable_order_hint)
{
    if (!enable_order_hint || order_hint_bits <= 0) return 0;

    const int m     = 1 << (order_hint_bits - 1);
    const int mask  = (1 << order_hint_bits) - 1;

    int diff = static_cast<int>(a) - static_cast<int>(b);
    diff &= mask;                 // keep only N bits
    if (diff & m) diff -= (m << 1);  // sign-extend
    return diff;
}

// ============================================================================
//  Minimal Header “Views” (what your parser provides)
// ============================================================================
// In your codebase, these will map to your DAV1D-style structs or adapters.
// We keep just the fields we need for DPB + offsets flow.
// ============================================================================

struct SequenceHeaderView {
    bool enable_order_hint{false};
    int  order_hint_bits{0}; // [0..8], typical <= 8
};

struct FrameHeaderView {
    // Whether bitstream asks to show an *existing* reference frame right away
    bool show_existing_frame{false};

    // If show_existing_frame==true: which slot [0..7] to show
    int  frame_to_show_map_idx{-1};

    // Whether this newly decoded frame is immediately shown
    bool show_frame{false};

    // Encoder-provided flag indicating the frame may be shown later
    bool showable_frame{false};

    // Mask that selects which slots [0..7] are overwritten by the current frame
    // when decoding a *new* frame (not for show_existing_frame paths).
    uint8_t refresh_frame_flags{0};

    // The order hint for this frame (lower N bits valid per sequence header)
    uint32_t order_hint{0};

    // The mapping (LAST..ALTREF) → DPB slot indices [0..7], chosen by
    // the "Set frame refs" process. -1 means unavailable/invalid.
    std::array<int8_t, REFS_PER_FRAME> ref_frame_idx{};
};

// ============================================================================
//  Buffer Table (FrameBuf) & DPBManager
// ============================================================================

struct FrameBuf {
    // Your decoded surface / buffer id (index into a pool)
    int       id{-1};

    // Cached order hint (LSBs valid per sequence header). Useful for debug
    // and in some pipelines for MV scaling & frame reordering heuristics.
    uint32_t  order_hint{0};

    // Reference count policy:
    //   +1 per DPB slot referencing this buffer
    //   +1 per active display reference (while on screen / handoff queue)
    int       ref_cnt{0};

    // “Showable” mirrors spec’s showable_frame (can be shown via show_existing)
    bool      showable{false};

    // Diagnostic: whether ever presented
    bool      ever_shown{false};
};

// Tracks the 8-slot reference map and ref-counts for the backing buffers.
struct DPBManager {
    // ref_frame_map[slot] = buffers[] index of referenced frame, or -1
    std::array<int, NUM_REF_FRAMES>      ref_frame_map{};
    // order_hint_slots[slot] = order_hint for current content of that slot
    std::array<uint32_t, NUM_REF_FRAMES> order_hint_slots{};

    // External buffer pool (owned by your allocator)
    FrameBuf* buffers{nullptr};
    int       buffers_count{0};

    void Reset() {
        ref_frame_map.fill(-1);
        order_hint_slots.fill(0);
    }

    // --- Retain/Release bookkeeping (no exceptions) ---
    void RetainBuf(int buf_id) {
        if (buf_id >= 0 && buf_id < buffers_count) ++buffers[buf_id].ref_cnt;
    }
    void ReleaseBuf(int buf_id) {
        if (buf_id >= 0 && buf_id < buffers_count) {
            auto& b = buffers[buf_id];
            if (b.ref_cnt > 0) --b.ref_cnt;
            // Optional: if (b.ref_cnt == 0) → return to free-list here.
        }
    }

    // Apply refresh flags for a *newly decoded* frame.
    // - Pins current buffer into each selected slot (increments ref_cnt)
    // - Releases old buffers previously pinned in those slots
    // - Updates per-slot order hints
    // - Handles showable/show_frame bookkeeping for the current buffer
    void ApplyRefreshFlags(int cur_buf_id,
                           uint32_t cur_order_hint,
                           uint8_t refresh_frame_flags,
                           bool show_frame,
                           bool showable_frame)
    {
        if (cur_buf_id < 0 || cur_buf_id >= buffers_count) return;

        buffers[cur_buf_id].order_hint = cur_order_hint;

        // Replace slots as requested by refresh_frame_flags
        for (int i = 0; i < NUM_REF_FRAMES; ++i) {
            if (((refresh_frame_flags >> i) & 1) == 0) continue;

            const int prev = ref_frame_map[i];
            if (prev != cur_buf_id) { // avoid self-release/spurious retain
                if (prev >= 0) ReleaseBuf(prev);
                ref_frame_map[i] = cur_buf_id;
                RetainBuf(cur_buf_id);
            }
            order_hint_slots[i] = cur_order_hint;
        }

        // If shown now, it generally should not remain showable
        buffers[cur_buf_id].showable   = showable_frame && !show_frame;
        buffers[cur_buf_id].ever_shown = buffers[cur_buf_id].ever_shown || show_frame;

        // If the frame is presented immediately, take a display ref.
        if (show_frame) {
            RetainBuf(cur_buf_id);
        }
    }

    // For show_existing_frame path: present an already-referenced slot
    // Returns a buffer id to display, or -1 if invalid.
    int ShowExistingFrame(int frame_to_show_map_idx) {
        if (frame_to_show_map_idx < 0 || frame_to_show_map_idx >= NUM_REF_FRAMES)
            return -1;

        const int buf_id = ref_frame_map[frame_to_show_map_idx];
        if (buf_id < 0 || buf_id >= buffers_count) return -1;

        RetainBuf(buf_id);                       // display hold
        buffers[buf_id].ever_shown = true;
        buffers[buf_id].showable   = false;      // once shown, no longer showable
        return buf_id;
    }

    // Release a previously retained display reference.
    void ReleaseDisplayRef(int buf_id) { ReleaseBuf(buf_id); }

    // Views for the offsets helper:
    const std::array<uint32_t, NUM_REF_FRAMES>& RefSlotOrderHints() const {
        return order_hint_slots;
    }
    const std::array<int, NUM_REF_FRAMES>& RefMap() const { return ref_frame_map; }
};

// ============================================================================
//  Reference Offsets (per-coded-ref distances & sign bias)
// ============================================================================

struct RefOffsets {
    // Signed temporal distance for each coded ref (LAST..ALTREF):
    //   d = get_relative_dist(ref_hint, cur_hint)
    std::array<int, REFS_PER_FRAME> offset{};

    // 0 = forward (ref before current), 1 = backward (ref after current)
    std::array<uint8_t, REFS_PER_FRAME> sign_bias{};
};

// Compute per-ref temporal distances & bias for current frame.
static inline void UpdateRefOffsets(uint32_t cur_order_hint,
                                    const std::array<uint32_t, NUM_REF_FRAMES>& ref_order_hint,
                                    const std::array<int8_t,  REFS_PER_FRAME>&  ref_frame_idx,
                                    int order_hint_bits,
                                    bool enable_order_hint,
                                    RefOffsets& out)
{
    for (int i = 0; i < REFS_PER_FRAME; ++i) {
        const int8_t idx = ref_frame_idx[i];
        if (idx < 0 || idx >= NUM_REF_FRAMES) {
            out.offset[i]    = 0;
            out.sign_bias[i] = 0;
            continue;
        }
        const uint32_t ref_hint = ref_order_hint[static_cast<int>(idx)];
        const int d = get_relative_dist(ref_hint, cur_order_hint, order_hint_bits, enable_order_hint);
        out.offset[i]    = d;
        out.sign_bias[i] = (d > 0) ? 1u : 0u; // backward if ref after current
    }
}

// ============================================================================
//  Decoder Integration Hooks
// ============================================================================

// A tiny “decode callback” interface: you provide an implementation that
// (1) resolves reference surfaces from the ref map indices,
// (2) decodes the current frame into a surface,
// (3) returns the buffer id of the decoded surface, or -1 on failure.
//
// The shim passes you:
//   - dpb.RefMap() so you can turn slot indices into concrete surfaces
//   - frame header fields (order hint bits, ref arrays, etc.)
//   - optional precomputed RefOffsets (temporal distances/sign-bias)
//
struct DecoderCallbacks {
    // Called to actually perform decode of a *new* frame.
    // Return a buffer id (index into DPBManager::buffers) on success, else -1.
    int (*decode_frame)(
        const DPBManager& dpb,
        const SequenceHeaderView& seq_hdr,
        const FrameHeaderView& frame_hdr,
        const RefOffsets* ref_offsets  // may be nullptr if you prefer to compute inside
    ) { nullptr };

    // Optional: called before decode to let the client map slots → surfaces
    // or validate the ref set. Return false to abort.
    bool (*on_before_decode)(
        const DPBManager& dpb,
        const SequenceHeaderView& seq_hdr,
        const FrameHeaderView& frame_hdr,
        const RefOffsets* ref_offsets
    ) { nullptr };

    // Optional: called after a frame is queued for display (show_frame or show_existing).
    void (*on_present)(
        int display_buf_id,
        const DPBManager& dpb,
        const SequenceHeaderView& seq_hdr,
        const FrameHeaderView& frame_hdr
    ) { nullptr };

    // Optional: error reporting hook (no exceptions in this file).
    void (*on_error)(const char* msg) { nullptr };
};

// ============================================================================
//  End-to-End: DecodeOneAccessUnit()
// ============================================================================
// Handles one AV1 "access unit" (frame) using the provided DPBManager,
// parsed headers, and decoder callbacks.
//
// Control flow:
//   A) If show_existing_frame:
//        - take display ref from the requested slot
//        - invoke on_present()
//        - caller must later call dpb.ReleaseDisplayRef(display_id)
//      (No refresh_frame_flags apply here; spec requires them to be zero.)
//
//   B) Else (new frame):
//        - compute RefOffsets for motion scaling & bias (optional but provided)
//        - on_before_decode() hook (optional)
//        - decode_frame() → returns cur_buf_id
//        - dpb.ApplyRefreshFlags(...)
//        - if show_frame: on_present()
//        - IMPORTANT: when display completes, caller must call dpb.ReleaseDisplayRef(cur_buf_id)
//
// Return value: display buffer id if something was queued for display this call,
//               otherwise -1. (You may have out-of-order display queues elsewhere.)
// ============================================================================
static inline int DecodeOneAccessUnit(DPBManager& dpb,
                                      const SequenceHeaderView& seq_hdr,
                                      const FrameHeaderView& frame_hdr,
                                      DecoderCallbacks& cbs)
{
    // ------------------------------------------------------------
    // Path A: show_existing_frame (no new decoding, just present)
    // ------------------------------------------------------------
    if (frame_hdr.show_existing_frame) {
        const int display_id = dpb.ShowExistingFrame(frame_hdr.frame_to_show_map_idx);
        if (display_id < 0) {
            if (cbs.on_error) cbs.on_error("show_existing_frame: invalid map index or empty slot");
            return -1;
        }
        if (cbs.on_present) cbs.on_present(display_id, dpb, seq_hdr, frame_hdr);
        return display_id; // Caller should dpb.ReleaseDisplayRef(display_id) when done
    }

    // ------------------------------------------------------------
    // Path B: decode a new frame
    // ------------------------------------------------------------

    // 1) Compute reference offsets/sign-bias for LAST..ALTREF (optional but handy)
    RefOffsets ro{};
    UpdateRefOffsets(frame_hdr.order_hint,
                     dpb.RefSlotOrderHints(),
                     frame_hdr.ref_frame_idx,
                     seq_hdr.order_hint_bits,
                     seq_hdr.enable_order_hint,
                     ro);

    // 2) Optional pre-decode hook (validate refs, prefetch surfaces, etc.)
    if (cbs.on_before_decode) {
        if (!cbs.on_before_decode(dpb, seq_hdr, frame_hdr, &ro)) {
            if (cbs.on_error) cbs.on_error("on_before_decode rejected the frame");
            return -1;
        }
    }

    // 3) Call the real decoder to produce a buffer
    if (!cbs.decode_frame) {
        if (cbs.on_error) cbs.on_error("decode_frame callback not provided");
        return -1;
    }

    const int cur_buf_id = cbs.decode_frame(dpb, seq_hdr, frame_hdr, &ro);
    if (cur_buf_id < 0 || cur_buf_id >= dpb.buffers_count) {
        if (cbs.on_error) cbs.on_error("decode_frame failed or returned invalid buf id");
        return -1;
    }

    // 4) Update DPB slots per refresh_frame_flags; handle showable/show_frame
    dpb.ApplyRefreshFlags(cur_buf_id,
                          frame_hdr.order_hint,
                          frame_hdr.refresh_frame_flags,
                          frame_hdr.show_frame,
                          frame_hdr.showable_frame);

    // 5) If immediately shown, fire present callback and return the display id.
    if (frame_hdr.show_frame) {
        if (cbs.on_present) cbs.on_present(cur_buf_id, dpb, seq_hdr, frame_hdr);
        return cur_buf_id; // Caller: dpb.ReleaseDisplayRef(cur_buf_id) when display completes
    }

    // Not displayed right now (could be shown later via show_existing_frame)
    return -1;
}

// ============================================================================
//  Minimal Example of decode_frame() Implementation Sketch (for integration)
// ============================================================================
// This is just an example function to show how you'd wire ref slots to
// surfaces for the actual decode. Replace with your real decoder.
// ============================================================================
static int ExampleDecodeFrameImpl(const DPBManager& dpb,
                                  const SequenceHeaderView& /*seq_hdr*/,
                                  const FrameHeaderView& /*frame_hdr*/,
                                  const RefOffsets* /*ro*/)
{
    // --- Map ref slots → surface ids as needed ---
    // For example, LAST frame surface id:
    // int last_slot = frame_hdr.ref_frame_idx[0];
    // int last_buf  = (last_slot >= 0) ? dpb.RefMap()[last_slot] : -1;
    // Use these surfaces for MC, warped motion, etc.

    // --- Acquire a free buffer from your pool (not shown here) ---
    // In this example we assume slot 0 in the pool is available:
    int chosen = 0;
    // Safety check: make sure it's inside range
    if (chosen < 0 || chosen >= dpb.buffers_count) return -1;

    // --- Perform the actual decode into buffers[chosen] ---
    // Your pipeline would write the frame’s pixels, set metadata, etc.

    return chosen; // Return the decoded buffer id
}

// ============================================================================
//  Optional: Wire up default callbacks quickly
// ============================================================================

static inline DecoderCallbacks MakeDefaultCallbacks() {
    DecoderCallbacks cbs{};
    cbs.decode_frame = &ExampleDecodeFrameImpl;
    cbs.on_before_decode = nullptr;
    cbs.on_present = nullptr;
    cbs.on_error = nullptr;
    return cbs;
}

// ============================================================================
//  Usage Sketch (outside this file, for reference)
// ============================================================================
//
//   // 1) Create/own a FrameBuf pool.
//   std::array<av1::FrameBuf, kPool> pool{};
//   for (int i = 0; i < kPool; ++i) { pool[i].id = i; }
//
//   // 2) Make a DPBManager and point it at the pool.
//   av1::DPBManager dpb;
//   dpb.buffers = pool.data();
//   dpb.buffers_count = static_cast<int>(pool.size());
//   dpb.Reset();
//
//   // 3) Prepare callbacks.
//   av1::DecoderCallbacks cbs = av1::MakeDefaultCallbacks();
//
//   // 4) For each Access Unit:
//   av1::SequenceHeaderView seq_hdr{ .enable_order_hint=true, .order_hint_bits=8 };
//   av1::FrameHeaderView    frame_hdr{ /* fill from parser */ };
//
//   //    (a) If frame_hdr.show_existing_frame, DecodeOneAccessUnit will present an existing slot.
//   //    (b) Otherwise it decodes a new frame, refreshes slots, and optionally presents.
//
//   int display_id = av1::DecodeOneAccessUnit(dpb, seq_hdr, frame_hdr, cbs);
//   if (display_id >= 0) {
//       // Hand off to compositor/output; when done:
//       dpb.ReleaseDisplayRef(display_id);
//   }
//
// ============================================================================

} // namespace av1

} // namespace av1

#endif // AV1_REFS_H_