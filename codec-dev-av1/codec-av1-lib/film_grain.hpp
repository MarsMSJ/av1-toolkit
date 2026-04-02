#pragma once

// Structures and functions sourced from AOM reference decoder:
// - aom/aom_dsp/grain_params.h
// - aom/av1/decoder/grain_synthesis.h

#include <cstdio>
#include <cstdint>
#include <array>
#include <cstring>
#include <vector>

namespace av1 {

// Equivalent to aom_film_grain_t in AOM reference code
// Specified in Section 6.8.20 (Film grain params syntax) of AV1 specification
struct FilmGrainParams {
    bool apply_grain = false;
    bool update_parameters = false;

    std::array<std::array<int, 2>, 14> scaling_points_y{};
    int num_y_points = 0; // 0..14

    std::array<std::array<int, 2>, 10> scaling_points_cb{};
    int num_cb_points = 0; // 0..10

    std::array<std::array<int, 2>, 10> scaling_points_cr{};
    int num_cr_points = 0; // 0..10

    int scaling_shift = 0; // 8..11
    int ar_coeff_lag = 0;  // 0..3

    std::array<int, 24> ar_coeffs_y{};
    std::array<int, 25> ar_coeffs_cb{};
    std::array<int, 25> ar_coeffs_cr{};

    int ar_coeff_shift = 0; // 6..9

    int cb_mult = 0;
    int cb_luma_mult = 0;
    int cb_offset = 0;

    int cr_mult = 0;
    int cr_luma_mult = 0;
    int cr_offset = 0;

    bool overlap_flag = false;
    bool clip_to_restricted_range = false;
    unsigned int bit_depth = 8;
    bool chroma_scaling_from_luma = false;
    int grain_scale_shift = 0;
    uint16_t random_seed = 0;
};

bool check_grain_params_equiv(const FilmGrainParams& pa, const FilmGrainParams& pb);

class FilmGrainSynthesizer {
public:
    FilmGrainSynthesizer() = default;

    int add_film_grain(const FilmGrainParams& params,
                       const std::vector<uint8_t>& src_y, int src_y_stride,
                       const std::vector<uint8_t>& src_cb, int src_cb_stride,
                       const std::vector<uint8_t>& src_cr, int src_cr_stride,
                       std::vector<uint8_t>& dst_y, int dst_y_stride,
                       std::vector<uint8_t>& dst_cb, int dst_cb_stride,
                       std::vector<uint8_t>& dst_cr, int dst_cr_stride,
                       int width, int height,
                       int chroma_subsamp_x, int chroma_subsamp_y);

private:
    uint16_t random_register_ = 0;

    static const int gaussian_sequence[2048];
    static const int gauss_bits = 11;

    static const int min_luma_legal_range   = 16;
    static const int max_luma_legal_range   = 235;
    static const int min_chroma_legal_range = 16;
    static const int max_chroma_legal_range = 240;

    // Fixed subblock size — same as AOM (luma_subblock_size_y/x = 32)
    static constexpr int luma_subblock_size_y = 32;
    static constexpr int luma_subblock_size_x = 32;

    int grain_min_ = 0;
    int grain_max_ = 0;

    int  get_random_number(int bits);
    void init_random_generator(int luma_line, uint16_t seed);

    // scaling_points is a pointer to {x, y} pairs — the type .data() returns on
    // both the 14-point (luma) and 10-point (chroma) arrays, so one function
    // handles both without caring about the compile-time max-point count.
    static void init_scaling_function(const std::array<int, 2>* scaling_points,
                                      int num_points, std::array<int, 256>& scaling_lut);
    static int  scale_LUT(const std::array<int, 256>& scaling_lut, int index, int bit_depth);

    // Grain template generation (Section 7.18.3.2)
    // pred_pos entries: [row_offset, col_offset, use_luma]
    void generate_luma_grain_block(
        const FilmGrainParams& params,
        const std::vector<std::array<int, 3>>& pred_pos_luma,
        int* luma_grain_block,
        int luma_block_size_y, int luma_block_size_x, int luma_grain_stride,
        int left_pad, int top_pad, int right_pad, int bottom_pad);

    void generate_chroma_grain_blocks(
        const FilmGrainParams& params,
        const std::vector<std::array<int, 3>>& pred_pos_chroma,
        const int* luma_grain_block,
        int* cb_grain_block, int* cr_grain_block,
        int luma_grain_stride,
        int chroma_block_size_y, int chroma_block_size_x, int chroma_grain_stride,
        int left_pad, int top_pad, int right_pad, int bottom_pad,
        int chroma_subsamp_y, int chroma_subsamp_x);

    // Noise application (Section 7.18.3.4) — in-place on frame pointers
    void add_noise_to_block(
        const FilmGrainParams& params,
        uint8_t* luma, uint8_t* cb, uint8_t* cr,
        int luma_stride, int chroma_stride,
        const int* luma_grain, const int* cb_grain, const int* cr_grain,
        int luma_grain_stride, int chroma_grain_stride,
        int half_luma_height, int half_luma_width,
        int bit_depth, int chroma_subsamp_y, int chroma_subsamp_x);

    // Overlap blending helpers (Section 7.18.3.5)
    static void ver_boundary_overlap(
        const int* left_block, int left_stride,
        const int* right_block, int right_stride,
        int* dst_block, int dst_stride,
        int width, int height,
        int grain_min, int grain_max);

    static void hor_boundary_overlap(
        const int* top_block, int top_stride,
        const int* bottom_block, int bottom_stride,
        int* dst_block, int dst_stride,
        int width, int height,
        int grain_min, int grain_max);

    static void copy_int_area(const int* src, int src_stride,
                              int* dst, int dst_stride,
                              int width, int height);

    std::array<int, 256> scaling_lut_y_{};
    std::array<int, 256> scaling_lut_cb_{};
    std::array<int, 256> scaling_lut_cr_{};
};

// =============================================================================
// Film grain debug instrumentation
//
// Prefix convention:
//   AV1_FG_DBG   — film grain pipeline  (this file)
//   AV1_SYN_DBG  — general syntax / DPB  (decoder layer, same pattern)
//
// Rules (same as aom_debug_instrument.py):
//   - All output goes to stderr, never stdout
//   - Every line is prefixed with [AV1_FG_DBG]
//   - Everything is compiled away unless AV1_DEBUG_FG is defined
//
// Enable at build time:
//   clang++ -DAV1_DEBUG_FG=1 ...
//   cmake  .. -DAOM_EXTRA_CXX_FLAGS="-DAV1_DEBUG_FG=1"
//
// Usage examples:
//   AV1_FG_DBG("apply_grain: frame=%d seed=%u", frame_idx, params.random_seed);
//   AV1_FG_DBG_DUMP_PARAMS(params);
//   AV1_FG_DBG_DUMP_GRAIN("luma", block.data(), stride, off_y, off_x, 64, 64);
// =============================================================================

// ---------------------------------------------------------------------------
// Underlying dump functions — call these directly when you need a specific
// FILE* (e.g. a log file).  The macros below always use stderr.
// ---------------------------------------------------------------------------

// Full [FILM_GRAIN] block — byte-identical to dump_film_grain() in
// aom/.../examples/av1dec_diag.c so you can diff our output against AOM.
void fg_dbg_dump_params(const FilmGrainParams& params, FILE* out = stderr);

// 2-D grid of a grain template sub-region.
//   label        — e.g. "luma", "cb", "cr"
//   block        — flat row-major grain array
//   grain_stride — row stride (luma_grain_stride / chroma_grain_stride)
//   offset_y/x   — top-left of the region (luma_offset_y/x or chroma_offset_y/x)
//   rows/cols    — region size  (64x64 luma, 32x32 chroma for 4:2:0)
void fg_dbg_dump_grain_block(const char* label,
                             const int* block, int grain_stride,
                             int offset_y, int offset_x,
                             int rows, int cols,
                             FILE* out = stderr);

} // namespace av1

// ---------------------------------------------------------------------------
// AV1_FG_DBG macros — zero-cost when AV1_DEBUG_FG is not defined
// ---------------------------------------------------------------------------

#ifdef AV1_DEBUG_FG

// Single-line tagged print.  fmt must be a string literal.
// Produces:  [AV1_FG_DBG] <message>\n  on stderr
// Example:   AV1_FG_DBG("seed=%u lag=%d", params.random_seed, params.ar_coeff_lag);
#  define AV1_FG_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_FG_DBG] " fmt "\n", ##__VA_ARGS__)

// Full params block (byte-identical to AOM av1dec_diag [FILM_GRAIN] output)
#  define AV1_FG_DBG_DUMP_PARAMS(params) \
       ::av1::fg_dbg_dump_params((params), stderr)

// Grain template region dump
// Usage: AV1_FG_DBG_DUMP_GRAIN("luma", block, stride, off_y, off_x, 64, 64)
#  define AV1_FG_DBG_DUMP_GRAIN(label, block, stride, off_y, off_x, rows, cols) \
       ::av1::fg_dbg_dump_grain_block((label), (block), (stride), \
                                      (off_y), (off_x), (rows), (cols), stderr)

#else // AV1_DEBUG_FG not set — compile everything away

#  define AV1_FG_DBG(fmt, ...)                                              ((void)0)
#  define AV1_FG_DBG_DUMP_PARAMS(params)                                    ((void)0)
#  define AV1_FG_DBG_DUMP_GRAIN(label, block, stride, off_y, off_x, rows, cols) ((void)0)

#endif // AV1_DEBUG_FG

// ---------------------------------------------------------------------------
// AV1_SYN_DBG — general syntax / DPB / reference frame debug prints
// Same pattern, separate flag so you can enable grain-only without the
// full syntax flood.
//
// Enable: -DAV1_DEBUG_SYNTAX=1
// Example: AV1_SYN_DBG("frame_type=%d show_frame=%d", ftype, show);
// ---------------------------------------------------------------------------

#ifdef AV1_DEBUG_SYNTAX
#  define AV1_SYN_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_SYN_DBG] " fmt "\n", ##__VA_ARGS__)
#else
#  define AV1_SYN_DBG(fmt, ...)  ((void)0)
#endif // AV1_DEBUG_SYNTAX
