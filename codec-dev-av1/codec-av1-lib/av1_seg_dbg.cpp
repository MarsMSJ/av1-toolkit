#include "av1_seg_dbg.h"

namespace av1::seg {

// [MMSJ] Feature names matching SegLvlFeature enum
static const char* kSegFeatureNames[kMax] = {
    "ALT_Q", "ALT_LF_Y_V", "ALT_LF_Y_H", "ALT_LF_U",
    "ALT_LF_V", "REF_FRAME", "SKIP", "GLOBALMV"
};

void segDbgDumpParams(const Av1SegmentationData& seg, FILE* out) {
    fprintf(out, "[SEGMENTATION]\n");
    fprintf(out, "  enabled          : %d\n", seg.enabled);
    if (!seg.enabled)
        return;

    fprintf(out, "  update_map       : %d\n", seg.update_map);
    fprintf(out, "  update_data      : %d\n", seg.update_data);
    fprintf(out, "  temporal_update  : %d\n", seg.temporal_update);
    fprintf(out, "  segid_preskip    : %d\n", seg.segid_preskip);
    fprintf(out, "  last_active_segid: %d\n", seg.last_active_segid);

    for (int i = 0; i < kMaxSegments; i++) {
        if (!seg.feature_mask[i])
            continue;
        fprintf(out, "  seg[%d] mask=0x%02x:", i, seg.feature_mask[i]);
        for (int f = 0; f < kMax; f++) {
            if (seg.feature_mask[i] & (1u << f))
                fprintf(out, " %s=%d", kSegFeatureNames[f], seg.feature_data[i][f]);
        }
        fprintf(out, "\n");
    }
}

}  // namespace av1::seg
