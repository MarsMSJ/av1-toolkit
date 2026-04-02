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

// Output of synthesizeFgSeg — scaling LUTs + grain templates.
// HW decoder receives the FilmGrainParams struct directly; this carries the
// pre-computed synthesis artefacts for validation or SW fallback.
struct FgSynthesisOutput {
    // Scaling LUTs (Section 7.18.3.2) — 256 entries, indexed by 8-bit pixel value.
    // For 10-bit, scale_LUT() interpolates between adjacent entries at lookup time.
    // chroma LUTs == luma LUT when chroma_scaling_from_luma is set.
    std::array<int, 256> scaling_lut_y{};
    std::array<int, 256> scaling_lut_cb{};
    std::array<int, 256> scaling_lut_cr{};

    // Grain templates — flat row-major, stride == block_size_x
    std::vector<int> luma_grain;
    std::vector<int> cb_grain;
    std::vector<int> cr_grain;

    // Template dimensions (luma_grain_stride == luma_block_size_x, same for chroma)
    int luma_block_size_y   = 0;
    int luma_block_size_x   = 0;
    int chroma_block_size_y = 0;
    int chroma_block_size_x = 0;
};

class FilmGrainSynthesizer {
public:
    FilmGrainSynthesizer() = default;

    // Compute scaling LUTs and grain templates from parsed film grain params.
    // Does NOT apply grain to pixels — that is the HW decoder's responsibility.
    // Returns 0 on success, -1 if apply_grain is false or params are invalid.
    [[nodiscard]] int synthesizeFgSeg(const FilmGrainParams& params,
                                      int chroma_subsamp_x, int chroma_subsamp_y,
                                      FgSynthesisOutput& out);

    // spec Section 7.18.3.3 — exposed for HW validation / debug
    static int scale_LUT(const std::array<int, 256>& scaling_lut,
                         int index, int bit_depth);

private:
    uint16_t random_register_ = 0;

    static const int gaussian_sequence[2048];
    static const int gauss_bits = 11;

    static constexpr int luma_subblock_size_y = 32;
    static constexpr int luma_subblock_size_x = 32;

    int grain_min_ = 0;
    int grain_max_ = 0;

    int  get_random_number(int bits);
    void init_random_generator(int luma_line, uint16_t seed);

    // scaling_points is a pointer to {x, y} pairs (.data() on either the
    // 14-point luma or 10-point chroma array) — one function handles both.
    static void init_scaling_function(const std::array<int, 2>* scaling_points,
                                      int num_points, std::array<int, 256>& scaling_lut);

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
//   AV1_FG_DBG_DUMP_GRAIN("luma", out.luma_grain.data(), out.luma_block_size_x, 0, 0, 64, 64);
// =============================================================================

// Full [FILM_GRAIN] block — byte-identical to dump_film_grain() in
// aom/.../examples/av1dec_diag.c so you can diff our output against AOM.
void fg_dbg_dump_params(const FilmGrainParams& params, FILE* out = stderr);

// 2-D grid of a grain template sub-region.
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

#  define AV1_FG_DBG(fmt, ...) \
       fprintf(stderr, "[AV1_FG_DBG] " fmt "\n", ##__VA_ARGS__)

#  define AV1_FG_DBG_DUMP_PARAMS(params) \
       ::av1::fg_dbg_dump_params((params), stderr)

#  define AV1_FG_DBG_DUMP_GRAIN(label, block, stride, off_y, off_x, rows, cols) \
       ::av1::fg_dbg_dump_grain_block((label), (block), (stride), \
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
