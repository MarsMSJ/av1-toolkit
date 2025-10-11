//
// Created by Mario M Sarria Jr on 10/9/25.
//

#ifndef AV1_DPB_MANAGER_H
#define AV1_DPB_MANAGER_H
#include <stdint.h>
#include "av1_dpb_support.h"
#include "av1-toolkit.h"


class Av1Dpb {
public:
    // ... (ctor/attach_pool/reset same as before)

    DpbStatus begin_frame(const Av1FrameInputs& in,
                          RefFrameMap& map,
                          RefFrameOffsets& offs,
                          Av1BeginResult& out)
    {
        // Initialize output structure
        out = {};
        out.pregrain_surf = av1::AV1_INVALID_FB_IDX;
        out.postgrain_surf = av1::AV1_INVALID_FB_IDX;
        out.display_surf = av1::AV1_INVALID_FB_IDX;

        // Resolve reference frames for motion compensation and entropy context
        // (regardless of show flags - these refs are needed for decoding)
        
        // Original implementation (commented for reference):
        // for (int i = 0; i < 7; ++i) {
        //     int8_t slot = in.ref_frames[i];
        //     if (slot < 0 || slot > 7) { out.refs.ref_surf[i] = -1; continue; }
        //     uint8_t sid = map.slot_to_surf[(size_t)slot];
        //     out.refs.ref_surf[i] = (sid == 0x7f) ? -1 : (int)sid;
        // }
        
        for (int ref_type = 0; ref_type < av1::AV1_REFS_PER_FRAME; ++ref_type) {
            const int8_t slot_idx = in.ref_frames[ref_type];
            
            // Check if reference slot index is valid (0-7)
            if (slot_idx < 0 || slot_idx > 7) {
                out.refs.ref_surf[ref_type] = av1::AV1_INVALID_FB_IDX;
                continue;
            }
            
            // Look up surface ID from the reference slot
            const uint8_t surface_id = map.slot_to_surf[static_cast<size_t>(slot_idx)];
            
            // Convert AV1 invalid reference (0x7F) to our API invalid value (-1)
            out.refs.ref_surf[ref_type] = (surface_id == av1::AV1_INVALID_SLOT_U8) 
                                        ? av1::AV1_INVALID_FB_IDX 
                                        : static_cast<int>(surface_id);
        }

        // Path A: show_existing_frame → display an existing slot, no new coded frame.
        if (in.show_existing_frame) {
            if (in.existing_frame_idx > 7) {return DpbStatus::InvalidExistingIndex;}

            uint8_t sid = map.slot_to_surf[in.existing_frame_idx];
            if (sid == 0x7f) return DpbStatus::InvalidExistingIndex;

            // If grain is applied at display time, allocate a post-grain target and set as display.
            if (in.apply_grain) {
                if (!pool) return DpbStatus::NeedMoreSurfaces;
                out.postgrain_surf = pool->alloc_postgrain();
                if (out.postgrain_surf < 0) return DpbStatus::NeedMoreSurfaces;
                // Caller will run grain: src = sid, dst = out.postgrain_surf
                out.display_surf = out.postgrain_surf;
                // No pre-grain allocation; we’re not decoding a new frame here.
            } else {
                // Display the referenced surface directly.
                out.display_surf = (int)sid;
            }

            // Spec: refresh_frame_flags must be 0 when show_existing_frame==1.
            // We’ll enforce a no-op refresh in commit().
            last_inputs = in;
            pending_order_hint = offs.order_hint[in.existing_frame_idx]; // for completeness
            path_show_existing_ = true;
            return DpbStatus::Ok;
        }

        // Path B: normal coded frame (may or may not be shown now)
        if (!pool) return DpbStatus::NeedMoreSurfaces;

        out.pregrain_surf = pool->alloc_pregrain();
        if (out.pregrain_surf < 0) return DpbStatus::NeedMoreSurfaces;

        if (in.apply_grain) {
            out.postgrain_surf = pool->alloc_postgrain();
            if (out.postgrain_surf < 0) return DpbStatus::NeedMoreSurfaces;
        }

        // If show_frame==1, decide the display target (post-grain if grain, otherwise pre-grain).
        if (in.show_frame) {
            out.display_surf = (in.apply_grain && out.postgrain_surf >= 0)
                               ? out.postgrain_surf
                               : out.pregrain_surf;
        }

        // Record for commit
        last_inputs = in;
        pending_order_hint = in.order_hint_curr;
        path_show_existing_ = false;
        return DpbStatus::Ok;
    }

    void commit_frame(RefFrameMap& map,
                      RefFrameOffsets& offs,
                      int final_output_surface_id /* pre- or post-grain chosen by caller */)
    {
        if (path_show_existing_) {
            // show_existing_frame: no refresh of DPB per spec.
            // If you must be permissive, mask refresh_frame_flags=0 here.
            return;
        }

        // Normal coded frame: refresh slots as signaled.
        const uint8_t flags = last_inputs.refresh_frame_flags;
        for (uint32_t slot = 0; slot < 8; ++slot) {
            if (flags & (1u << slot)) {
                maybe_release_surface(map.slot_to_surf[slot], final_output_surface_id);
                map.slot_to_surf[slot] = (uint8_t)final_output_surface_id;
                offs.order_hint[slot]  = pending_order_hint;
            }
        }

        // Optionally: if the coded frame is "showable", you may pin one slot to it even if flags==0.
        // Many implementations map "showable_frame" to keeping at least one alias for show-existing later.
        if (last_inputs.frame_is_showable && flags == 0) {
            // Pick a policy slot (e.g., LAST) only if empty; otherwise do nothing.
            constexpr uint32_t kPolicySlot = 0; // LAST
            if (map.slot_to_surf[kPolicySlot] == 0x7f) {
                map.slot_to_surf[kPolicySlot] = (uint8_t)final_output_surface_id;
                offs.order_hint[kPolicySlot]  = pending_order_hint;
            }
        }
    }

private:
    SurfacePool* pool = nullptr;
    Av1FrameInputs last_inputs{};
    int32_t pending_order_hint = 0;
    bool path_show_existing_ = false;

    void maybe_release_surface(uint8_t old_surf, int new_surf) {
        if (!pool) return;
        if (old_surf == 0x7f) return;
        if ((int)old_surf == new_surf) return;
        // NOTE: for real-world use, implement reference counts because multiple slots can alias one surface.
        pool->release(old_surf);
    }
};

#ifdef 0
//
// --- Main DPB Manager --------------------------------------------------------
//

// This class models AV1's decoded picture buffer management for one sequence.
// It owns no memory — it uses an external SurfacePool and external ref maps.
// Film grain support can later use the extra two surfaces in SurfacePool.
class Av1Dpb {
public:
    explicit Av1Dpb(SurfacePool* p = nullptr) : pool(p) {}

    //---------------------------------------------------------------------------
    // reset()
    //
    // Clears all reference slots and releases their surfaces back to the pool.
    // Typically called on a key frame with "refresh_frame_flags == 0xFF"
    // or when starting a new sequence.
    //---------------------------------------------------------------------------
    static void reset(RefFrameMap& map, RefFrameOffsets& offs, SurfacePool* pool) {
        for (auto &m : map.slot_to_surf) {
            if (m != 0x7f && pool) pool->release(m);
            m = 0x7f;
        }
        offs.order_hint.fill(0);
    }

    //---------------------------------------------------------------------------
    // begin_frame()
    //
    // Resolves reference surfaces and allocates working surfaces
    // for the next access unit.
    //
    // Inputs:
    //   - in:   parsed frame header
    //   - map:  current ref slot → surface mapping
    //   - offs: order_hint table (used for show_existing_frame)
    //
    // Output:
    //   - out.refs:  surfaces for MC references
    //   - out.work_surf: new decode target (if any)
    //   - out.display_surf: surface to be shown (if any)
    //
    // Behavior:
    //   1) show_existing_frame == 1 → display an old slot, no new alloc.
    //   2) otherwise allocates one work surface for decoding.
    //      If show_frame == 1, marks it as the display surface.
    //---------------------------------------------------------------------------
    DpbStatus begin_frame(const Av1FrameInputs& in,
                          const RefFrameMap& map,
                          const RefFrameOffsets& offs,
                          Av1BeginResult& out)
    {
        // Resolve 7 reference surfaces using the current map.
        for (int i = 0; i < 7; ++i) {
            int8_t slot = in.ref_frames[i];
            out.refs.ref_surf[i] = (slot < 0 || slot > 7)
                                   ? -1
                                   : (map.slot_to_surf[(size_t)slot] == 0x7f
                                      ? -1
                                      : (int)map.slot_to_surf[(size_t)slot]);
        }

        // --- Case 1: show_existing_frame (no new coded frame) ---
        if (in.show_existing_frame) {
            if (in.existing_frame_idx > 7) return DpbStatus::InvalidExistingIndex;
            uint8_t sid = map.slot_to_surf[in.existing_frame_idx];
            if (sid == 0x7f) return DpbStatus::InvalidExistingIndex;

            out.work_surf    = -1;       // nothing to decode
            out.display_surf = (int)sid; // reuse an existing surface

            path_show_existing_ = true;
            pending_order_hint_ = offs.order_hint[in.existing_frame_idx];
            last_in_ = in;
            return DpbStatus::Ok;
        }

        // --- Case 2: normal coded frame ---
        if (!pool) return DpbStatus::NeedMoreSurfaces;
        out.work_surf = pool->alloc();
        if (out.work_surf < 0) return DpbStatus::NeedMoreSurfaces;

        // If this frame is to be displayed now, mark it as such.
        out.display_surf = in.show_frame ? out.work_surf : -1;

        path_show_existing_ = false;
        pending_order_hint_ = in.order_hint_curr;
        last_in_ = in;
        return DpbStatus::Ok;
    }

    //---------------------------------------------------------------------------
    // commit_frame()
    //
    // Updates the DPB after a frame is fully decoded and reconstructed.
    //
    // Inputs:
    //   - map, offs: reference slot tables to be updated
    //   - final_surface_id: surface ID of the completed decoded frame
    //
    // Behavior:
    //   - If show_existing_frame was active, does nothing (spec: no refresh).
    //   - Otherwise, refreshes any slots marked in refresh_frame_flags.
    //   - Each refreshed slot now points to final_surface_id.
    //---------------------------------------------------------------------------
    void commit_frame(RefFrameMap& map, RefFrameOffsets& offs, int final_surface_id)
    {
        if (path_show_existing_) {
            // show_existing_frame → no DPB refresh
            return;
        }

        const uint8_t flags = last_in_.refresh_frame_flags;
        for (uint32_t slot = 0; slot < 8; ++slot) {
            if (flags & (1u << slot)) {
                maybe_release(map.slot_to_surf[slot], final_surface_id);
                map.slot_to_surf[slot] = (uint8_t)final_surface_id;
                offs.order_hint[slot]  = pending_order_hint_;
            }
        }
    }

private:
    SurfacePool* pool{};             // external pool for surface allocation
    Av1FrameInputs last_in_{};       // cached frame header for commit phase
    int32_t pending_order_hint_{};   // saved order_hint of current frame
    bool path_show_existing_{false}; // true if last frame was show_existing_frame

    //---------------------------------------------------------------------------
    // maybe_release()
    //
    // Releases an old surface if it is being replaced by a new one in the map.
    // Real decoders should use refcounting; here we assume 1:1 mapping.
    //---------------------------------------------------------------------------
    void maybe_release(uint8_t old_surf, int new_surf) {
        if (!pool) return;
        if (old_surf == 0x7f) return;
        if ((int)old_surf == new_surf) return; // same backing surface
        pool->release(old_surf);
    }
};
#endif

#endif //AV1_DPB_MANAGER_H