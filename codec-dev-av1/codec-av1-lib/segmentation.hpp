#pragma once

// Constants and structures sourced from AOM reference decoder: aom/av1/common/seg_common.h

#include <cstdint>
#include <array>
#include <cstring>

namespace av1 {

constexpr int MAX_SEGMENTS = 8;
constexpr int SEG_TREE_PROBS = (MAX_SEGMENTS - 1);
constexpr int SEG_TEMPORAL_PRED_CTXS = 3;
constexpr int SPATIAL_PREDICTION_PROBS = 3;

enum SegLvlFeatures {
  SEG_LVL_ALT_Q,       // Use alternate Quantizer ....
  SEG_LVL_ALT_LF_Y_V,  // Use alternate loop filter value on y plane vertical
  SEG_LVL_ALT_LF_Y_H,  // Use alternate loop filter value on y plane horizontal
  SEG_LVL_ALT_LF_U,    // Use alternate loop filter value on u plane
  SEG_LVL_ALT_LF_V,    // Use alternate loop filter value on v plane
  SEG_LVL_REF_FRAME,   // Optional Segment reference frame
  SEG_LVL_SKIP,        // Optional Segment (0,0) + skip mode
  SEG_LVL_GLOBALMV,
  SEG_LVL_MAX
};

// Equivalent to struct segmentation in AOM reference code
// Specified in Section 6.8.14 (Segmentation params syntax) of AV1 specification
struct SegmentationData {
  bool enabled = false;
  bool update_map = false;
  bool update_data = false;
  bool temporal_update = false;

  std::array<std::array<int16_t, SEG_LVL_MAX>, MAX_SEGMENTS> feature_data{};
  std::array<unsigned int, MAX_SEGMENTS> feature_mask{};
  
  int last_active_segid = 0;  // The highest numbered segment id that has some enabled feature.
  bool segid_preskip = false; // Whether the segment id will be read before the skip syntax element.
};

bool segfeature_active(const SegmentationData& seg, uint8_t segment_id, SegLvlFeatures feature_id);
void segfeatures_copy(SegmentationData& dst, const SegmentationData& src);
void clearall_segfeatures(SegmentationData& seg);
void enable_segfeature(SegmentationData& seg, int segment_id, SegLvlFeatures feature_id);
void calculate_segdata(SegmentationData& seg);
void set_segdata(SegmentationData& seg, int segment_id, SegLvlFeatures feature_id, int seg_data);
int get_segdata(const SegmentationData& seg, int segment_id, SegLvlFeatures feature_id);

} // namespace av1
