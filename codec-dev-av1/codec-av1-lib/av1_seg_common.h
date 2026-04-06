#pragma once

// [MMSJ] §5.9.14 / AOM: av1/common/seg_common.h

#include <array>
#include <cstdint>

namespace av1::seg {

// [MMSJ] §5.9.14
constexpr int kMaxSegments          = 8;
constexpr int kSegTreeProbs         = kMaxSegments - 1;
constexpr int kSegTemporalPredCtxs  = 3;
constexpr int kSpatialPredictionProbs = 3;

// [MMSJ] AOM: av1/common/quant_common.h, av1/common/av1_loopfilter.h
constexpr int kMaxQ          = 255;
constexpr int kMaxLoopFilter = 63;

// [MMSJ] §5.9.14

enum SegLvlFeature {
    kAltQ      = 0,  // Alternate quantizer delta
    kAltLfYV   = 1,  // Alternate loop filter — Y vertical
    kAltLfYH   = 2,  // Alternate loop filter — Y horizontal
    kAltLfU    = 3,  // Alternate loop filter — U plane
    kAltLfV    = 4,  // Alternate loop filter — V plane
    kRefFrame  = 5,  // Segment reference frame
    kSkip      = 6,  // Skip mode + (0,0) mv
    kGlobalMv  = 7,  // Global motion
    kMax       = 8
};

// [MMSJ] AOM: av1/common/seg_common.c — seg_feature_data_signed
extern const std::array<int, kMax> kSegFeatureDataSigned;

// [MMSJ] AOM: av1/common/seg_common.c — seg_feature_data_max
extern const std::array<int, kMax> kSegFeatureDataMax;

// [MMSJ] §5.9.14 / AOM: struct segmentation

struct Av1SegmentationData {
    int  enabled         = 0;
    int  update_map      = 0;
    int  update_data     = 0;
    int  temporal_update = 0;

    // feature_data[segment_id][feature_id] — the delta/value for each feature
    std::array<std::array<int16_t, kMax>, kMaxSegments> feature_data{};

    // Bitmask per segment: bit N set means feature N is enabled
    std::array<unsigned int, kMaxSegments> feature_mask{};

    // Highest segment id with any enabled feature
    int last_active_segid = 0;

    // Whether segment id is read before the skip syntax element
    int segid_preskip = 0;

    // [MMSJ] AOM: segfeature_active
    bool isFeatureActive(int segment_id, SegLvlFeature feature_id) const {
        if (segment_id < 0 || segment_id >= kMaxSegments ||
            feature_id < 0 || feature_id >= kMax)
            return false;
        return enabled && (feature_mask[segment_id] & (1u << feature_id));
    }

    void enableFeature(int segment_id, SegLvlFeature feature_id) {
        if (segment_id < 0 || segment_id >= kMaxSegments ||
            feature_id < 0 || feature_id >= kMax)
            return;
        feature_mask[segment_id] |= (1u << feature_id);
    }

    void setData(int segment_id, SegLvlFeature feature_id, int value) {
        if (segment_id < 0 || segment_id >= kMaxSegments ||
            feature_id < 0 || feature_id >= kMax)
            return;
        feature_data[segment_id][feature_id] = static_cast<int16_t>(value);
    }

    int getData(int segment_id, SegLvlFeature feature_id) const {
        if (segment_id < 0 || segment_id >= kMaxSegments ||
            feature_id < 0 || feature_id >= kMax)
            return 0;
        return feature_data[segment_id][feature_id];
    }

    void clearAllFeatures() {
        for (int i = 0; i < kMaxSegments; i++) {
            feature_mask[i] = 0;
            feature_data[i].fill(0);
        }
        last_active_segid = 0;
    }

    // [MMSJ] AOM: av1_calculate_segdata
    void calculateLastActive() {
        last_active_segid = 0;
        for (int i = 0; i < kMaxSegments; i++) {
            if (feature_mask[i])
                last_active_segid = i;
        }
    }

    // [MMSJ] AOM: av1_segfeatures_copy
    void copyFeaturesFrom(const Av1SegmentationData& src) {
        feature_data      = src.feature_data;
        feature_mask      = src.feature_mask;
        last_active_segid = src.last_active_segid;
        segid_preskip     = src.segid_preskip;
    }

    // [MMSJ] Clamp to range from kSegFeatureDataSigned/kSegFeatureDataMax
    static int clampFeatureValue(SegLvlFeature feature_id, int value) {
        const int max_val = kSegFeatureDataMax[feature_id];
        if (kSegFeatureDataSigned[feature_id])
            return value < -max_val ? -max_val : value > max_val ? max_val : value;
        else
            return value < 0 ? 0 : value > max_val ? max_val : value;
    }

    // [MMSJ] AOM: av1_is_segfeature_signed
    static bool isFeatureSigned(SegLvlFeature feature_id) {
        return kSegFeatureDataSigned[feature_id] != 0;
    }

    // [MMSJ] AOM: av1_seg_feature_data_max
    static int featureDataMax(SegLvlFeature feature_id) {
        return kSegFeatureDataMax[feature_id];
    }
};

}  // namespace av1::seg
