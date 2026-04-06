#pragma once

// [MMSJ] Segmentation debug instrumentation

#include <cstdio>
#include "av1_seg_common.h"

namespace av1::seg {

void segDbgDumpParams(const Av1SegmentationData& seg, FILE* out = stderr);

}  // namespace av1::seg

#ifdef AV1_DEBUG_SEG

#  define AV1_SEG_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_SEG_DBG] " fmt "\n", ##__VA_ARGS__)

#  define AV1_SEG_DBG_DUMP_PARAMS(seg) \
       ::av1::seg::segDbgDumpParams((seg), stderr)

#else

#  define AV1_SEG_DBG(fmt, ...)          ((void)0)
#  define AV1_SEG_DBG_DUMP_PARAMS(seg)   ((void)0)

#endif
