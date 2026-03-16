#include "segmentation.hpp"

namespace av1 {

// Sourced from segfeature_active in aom/av1/common/seg_common.h
bool segfeature_active(const SegmentationData& seg, uint8_t segment_id, SegLvlFeatures feature_id) {
    if (segment_id >= MAX_SEGMENTS || feature_id >= SEG_LVL_MAX) return false;
    return seg.enabled && (seg.feature_mask[segment_id] & (1u << feature_id));
}

void segfeatures_copy(SegmentationData& dst, const SegmentationData& src) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        dst.feature_mask[i] = src.feature_mask[i];
        for (int j = 0; j < SEG_LVL_MAX; j++) {
            dst.feature_data[i][j] = src.feature_data[i][j];
        }
    }
    dst.segid_preskip = src.segid_preskip;
    dst.last_active_segid = src.last_active_segid;
}

void clearall_segfeatures(SegmentationData& seg) {
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        seg.feature_mask[i] = 0;
        for (int j = 0; j < SEG_LVL_MAX; j++) {
            seg.feature_data[i][j] = 0;
        }
    }
    seg.last_active_segid = 0;
}

void enable_segfeature(SegmentationData& seg, int segment_id, SegLvlFeatures feature_id) {
    if (segment_id < 0 || segment_id >= MAX_SEGMENTS || feature_id < 0 || feature_id >= SEG_LVL_MAX) return;
    seg.feature_mask[segment_id] |= (1u << feature_id);
}

void calculate_segdata(SegmentationData& seg) {
    seg.last_active_segid = 0;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        if (seg.feature_mask[i]) {
            seg.last_active_segid = i;
        }
    }
}

void set_segdata(SegmentationData& seg, int segment_id, SegLvlFeatures feature_id, int seg_data) {
    if (segment_id < 0 || segment_id >= MAX_SEGMENTS || feature_id < 0 || feature_id >= SEG_LVL_MAX) return;
    seg.feature_data[segment_id][feature_id] = static_cast<int16_t>(seg_data);
}

int get_segdata(const SegmentationData& seg, int segment_id, SegLvlFeatures feature_id) {
    if (segment_id < 0 || segment_id >= MAX_SEGMENTS || feature_id < 0 || feature_id >= SEG_LVL_MAX) return 0;
    return seg.feature_data[segment_id][feature_id];
}

} // namespace av1
