
#pragma once
#include <array>
#include <cstdint>

//
// --- DPB Support Structures --------------------------------------------------
//

// Holds the 8 standard AV1 reference slots.
// Each entry points to a surface index in the SurfacePool or 0x7f if invalid.
struct RefFrameMap {
    std::array<uint8_t, 8> slot_to_surf; // [0..7] = LAST..ALT3, etc.
};

// Stores per-slot order hints (frame presentation ordering info).
struct RefFrameOffsets {
    std::array<int32_t, 8> order_hint;
};

// Represents one decoded picture buffer surface.
// In a full decoder this would include Y/U/V planes, strides, etc.
struct Surface {
    uint32_t id{};     // Unique ID inside the pool
    uint16_t w{}, h{}; // Frame dimensions (optional)
    uint8_t  in_use{}; // 1 if allocated / referenced
};

// Fixed-size surface pool: 8 reference + 2 spare for future film grain support.
struct SurfacePool {
    static constexpr uint32_t kTotal = 10;
    std::array<Surface, kTotal> s{};

    SurfacePool() {
        for (uint32_t i = 0; i < kTotal; ++i) {
            s[i].id = i;
            s[i].in_use = 0;
        }
    }

    // Allocates the first free surface and marks it in use.
    int alloc() {
        for (auto &x : s)
            if (!x.in_use) { x.in_use = 1; return (int)x.id; }
        return -1; // none available
    }

    // Releases a surface back to the pool.
    void release(uint32_t id) { if (id < kTotal) s[id].in_use = 0; }
};

//
// --- Frame Header Inputs -----------------------------------------------------
//

// Simplified per-frame inputs used by the DPB manager.
// These values come from the parsed sequence + frame headers.
struct Av1FrameInputs {
    uint8_t  frame_type;             // KEY, INTER, INTRA_ONLY, etc.
    uint8_t  show_existing_frame;    // 1 if this frame displays an existing slot
    uint8_t  show_frame;             // 1 if this newly decoded frame is to be shown
    uint8_t  existing_frame_idx;     // Slot index [0..7] when show_existing_frame=1
    uint8_t  refresh_frame_flags;    // Bitmask selecting which slots are refreshed
    uint8_t  ref_frame_sign_bias[8]; // Copied from sequence header; used elsewhere
    int8_t   ref_frames[7];          // Reference slot indices for motion compensation
    int32_t  order_hint_curr;        // Current frame order_hint value
    uint16_t width, height;          // Frame dimensions
};

// Holds resolved surface indices for the seven reference frame types.
struct Av1RefsResolved { int ref_surf[7]; };

// Status codes returned by DPB operations.
enum class DpbStatus : uint8_t { Ok, NeedMoreSurfaces, InvalidExistingIndex };

//
// --- Begin-Frame Output ------------------------------------------------------
//

// Result of DPB::begin_frame(): lists references, working surface, display target.
struct Av1BeginResult {
    Av1RefsResolved refs;
    int pregrain_surf;     // -1 if not allocated
    int postgrain_surf;    // -1 if not allocated
    int display_surf;      // -1 if nothing to display yet; set when show_existing_frame or show_frame (+grain) resolves
/*
struct Av1FrameInputs {
    uint8_t  frame_type;             // KEY/INTER/...
    uint8_t  show_existing_frame;    // 0/1
    uint8_t  show_frame;             // 0/1  (display the newly coded frame now)
    uint8_t  existing_frame_idx;     // 0..7 if show_existing_frame==1
    uint8_t  refresh_frame_flags;    // 8-bit mask
    uint8_t  apply_grain;            // 0/1 (film grain synthesis required)
    uint8_t  frame_is_showable;      // 0/1 (spec showable_frame)
    uint8_t  ref_frame_sign_bias[8]; // pass-through for MC
    int8_t   ref_frames[7];          // indices into 8 slots, -1 invalid
    int32_t  order_hint_curr;        // current frame order hint
    uint16_t width, height;
};


};
*/