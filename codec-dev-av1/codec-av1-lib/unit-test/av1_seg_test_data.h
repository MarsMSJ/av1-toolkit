#pragma once

// [MMSJ] Segmentation test vectors for portable unit testing.
// Represents common segmentation configurations seen in real AV1 streams.

#include "../av1_seg_common.h"

namespace av1::seg::test {

// Basic: single segment with alt quantizer delta
inline Av1SegmentationData singleSegAltQ() {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.update_map = 1;
    seg.update_data = 1;
    seg.enableFeature(0, kAltQ);
    seg.setData(0, kAltQ, -15);
    seg.calculateLastActive();
    return seg;
}

// Multi-segment: 4 segments with different QP deltas (common in ROI encoding)
inline Av1SegmentationData multiSegROI() {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.update_map = 1;
    seg.update_data = 1;
    // Background
    seg.enableFeature(0, kAltQ);
    seg.setData(0, kAltQ, 0);
    // ROI low priority
    seg.enableFeature(1, kAltQ);
    seg.setData(1, kAltQ, -10);
    // ROI high priority
    seg.enableFeature(2, kAltQ);
    seg.setData(2, kAltQ, -25);
    // Skip region
    seg.enableFeature(3, kSkip);
    seg.calculateLastActive();
    return seg;
}

// Full features: segment with alt Q + alt loop filter + ref frame
inline Av1SegmentationData fullFeatures() {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.update_map = 1;
    seg.update_data = 1;
    seg.temporal_update = 1;
    seg.segid_preskip = 1;

    // Segment 0: background, no changes
    // Segment 1: lower QP, stronger LF
    seg.enableFeature(1, kAltQ);
    seg.setData(1, kAltQ, -20);
    seg.enableFeature(1, kAltLfYV);
    seg.setData(1, kAltLfYV, 5);
    seg.enableFeature(1, kAltLfYH);
    seg.setData(1, kAltLfYH, 5);

    // Segment 2: forced reference frame
    seg.enableFeature(2, kRefFrame);
    seg.setData(2, kRefFrame, 3);

    // Segment 5: global MV
    seg.enableFeature(5, kGlobalMv);

    // Segment 7: max QP delta + all LF planes
    seg.enableFeature(7, kAltQ);
    seg.setData(7, kAltQ, 50);
    seg.enableFeature(7, kAltLfYV);
    seg.setData(7, kAltLfYV, -10);
    seg.enableFeature(7, kAltLfYH);
    seg.setData(7, kAltLfYH, -10);
    seg.enableFeature(7, kAltLfU);
    seg.setData(7, kAltLfU, -5);
    seg.enableFeature(7, kAltLfV);
    seg.setData(7, kAltLfV, -5);

    seg.calculateLastActive();
    return seg;
}

// Edge case: all 8 segments with skip enabled
inline Av1SegmentationData allSegmentsSkip() {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.update_map = 1;
    seg.update_data = 1;
    for (int i = 0; i < kMaxSegments; i++)
        seg.enableFeature(i, kSkip);
    seg.calculateLastActive();
    return seg;
}

// Edge case: boundary values at feature limits
inline Av1SegmentationData boundaryValues() {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.update_map = 1;
    seg.update_data = 1;

    // Max positive QP delta
    seg.enableFeature(0, kAltQ);
    seg.setData(0, kAltQ, 255);  // kMaxQ

    // Max negative QP delta
    seg.enableFeature(1, kAltQ);
    seg.setData(1, kAltQ, -255);

    // Max loop filter
    seg.enableFeature(2, kAltLfYV);
    seg.setData(2, kAltLfYV, 63); // kMaxLoopFilter

    // Max negative loop filter
    seg.enableFeature(3, kAltLfYV);
    seg.setData(3, kAltLfYV, -63);

    // Max ref frame
    seg.enableFeature(4, kRefFrame);
    seg.setData(4, kRefFrame, 7);

    seg.calculateLastActive();
    return seg;
}

// Disabled segmentation — all operations should be no-ops
inline Av1SegmentationData disabled() {
    Av1SegmentationData seg{};
    seg.enabled = 0;
    // Features are set but should be inactive since enabled=0
    seg.enableFeature(0, kAltQ);
    seg.setData(0, kAltQ, -30);
    return seg;
}

}  // namespace av1::seg::test
