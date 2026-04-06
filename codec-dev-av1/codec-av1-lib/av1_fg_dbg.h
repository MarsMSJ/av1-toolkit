#pragma once

// AV1 film grain debug instrumentation
//
// Prefix convention:
//   AV1_FG_DBG   — film grain pipeline
//   AV1_SYN_DBG  — general syntax / DPB
//
// Rules:
//   - All output goes to stderr, never stdout
//   - Every line is prefixed with [AV1_FG_DBG] or [AV1_SYN_DBG]
//   - Everything compiles away unless AV1_DEBUG_FG / AV1_DEBUG_SYNTAX is defined
//
// Enable at build time:
//   clang++ -DAV1_DEBUG_FG=1 ...

#include <cstdio>
#include "av1_fg_common.h"

namespace av1::fg {

// Full [FILM_GRAIN] block — byte-identical to dump_film_grain() in
// aom/.../examples/av1dec_diag.c so you can diff output against AOM.
void fgDbgDumpParams(const Av1FilmGrainSynthesisData& params, FILE* out = stderr);

// 2-D grid of a grain template sub-region.
void fgDbgDumpGrainBlock(const char* label,
                         const int* block, int grain_stride,
                         int offset_y, int offset_x,
                         int rows, int cols,
                         FILE* out = stderr);

}  // namespace av1::fg

// ---------------------------------------------------------------------------
// AV1_FG_DBG macros — zero-cost when AV1_DEBUG_FG is not defined
// ---------------------------------------------------------------------------

#ifdef AV1_DEBUG_FG

#  define AV1_FG_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_FG_DBG] " fmt "\n", ##__VA_ARGS__)

#  define AV1_FG_DBG_DUMP_PARAMS(params) \
       ::av1::fg::fgDbgDumpParams((params), stderr)

#  define AV1_FG_DBG_DUMP_GRAIN(label, block, stride, off_y, off_x, rows, cols) \
       ::av1::fg::fgDbgDumpGrainBlock((label), (block), (stride), \
                                      (off_y), (off_x), (rows), (cols), stderr)

#else

#  define AV1_FG_DBG(fmt, ...)                                              ((void)0)
#  define AV1_FG_DBG_DUMP_PARAMS(params)                                    ((void)0)
#  define AV1_FG_DBG_DUMP_GRAIN(label, block, stride, off_y, off_x, rows, cols) ((void)0)

#endif // AV1_DEBUG_FG

// ---------------------------------------------------------------------------
// AV1_SYN_DBG — general syntax / DPB / reference frame debug prints
// Enable: -DAV1_DEBUG_SYNTAX=1
// ---------------------------------------------------------------------------

#ifdef AV1_DEBUG_SYNTAX
#  define AV1_SYN_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_SYN_DBG] " fmt "\n", ##__VA_ARGS__)
#else
#  define AV1_SYN_DBG(fmt, ...)  ((void)0)
#endif