#include "av1_seg_common.h"

namespace av1::seg {

// [MMSJ] AOM: av1/common/seg_common.c — seg_feature_data_signed
const std::array<int, kMax> kSegFeatureDataSigned = {
    1, 1, 1, 1, 1, 0, 0, 0
};

// [MMSJ] AOM: av1/common/seg_common.c — seg_feature_data_max
const std::array<int, kMax> kSegFeatureDataMax = {
    kMaxQ, kMaxLoopFilter, kMaxLoopFilter, kMaxLoopFilter,
    kMaxLoopFilter, 7, 0, 0
};

}  // namespace av1::seg
