#pragma once

// [MMSJ] Film grain test vectors adapted from AOM: av1/encoder/grain_test_vectors.h
// Converted to Av1FilmGrainSynthesisData for portable unit testing.

#include "../av1_fg_common.h"

namespace av1::fg::test {

// AOM Test 1: Complex curve, lag=2, clip_to_restricted_range=1
inline Av1FilmGrainSynthesisData aomVector1() {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 1;
    p.update_parameters = 1;
    p.num_y_points = 14;
    p.scaling_points_y = {{
        {16,0}, {25,136}, {33,144}, {41,160}, {48,168}, {56,136}, {67,128},
        {82,144}, {97,152}, {113,144}, {128,176}, {143,168}, {158,176}, {178,184}
    }};
    p.chroma_scaling_from_luma = 0;
    p.num_cb_points = 8;
    p.scaling_points_cb = {{
        {16,0}, {20,64}, {28,88}, {60,104}, {90,136}, {105,160}, {134,168}, {168,208}
    }};
    p.num_cr_points = 9;
    p.scaling_points_cr = {{
        {16,0}, {28,96}, {56,80}, {66,96}, {80,104}, {108,96}, {122,112}, {137,112}, {169,176}
    }};
    p.ar_coeff_lag = 2;
    p.ar_coeffs_y = {0, 0, -58, 0, 0, 0, -76, 100, -43, 0, -51, 82};
    p.ar_coeffs_cb = {0, 0, -49, 0, 0, 0, -36, 22, -30, 0, -38, 7, 39};
    p.ar_coeffs_cr = {0, 0, -47, 0, 0, 0, -31, 31, -25, 0, -32, 13, -100};
    p.ar_coeff_shift = 2;   // actual = 6+2 = 8
    p.cb_mult = 247; p.cb_luma_mult = 192; p.cb_offset = 18;
    p.cr_mult = 229; p.cr_luma_mult = 192; p.cr_offset = 54;
    p.scaling_shift = 4;    // actual = 7+4 = 11
    p.overlap_flag = 0;
    p.clip_to_restricted_range = 1;
    p.bit_depth = 8;
    p.grain_scale_shift = 0;
    p.random_seed = 45231;
    return p;
}

// AOM Test 2: Flat curve, lag=3, overlap=1, full range
inline Av1FilmGrainSynthesisData aomVector2() {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 1;
    p.update_parameters = 1;
    p.num_y_points = 2;
    p.scaling_points_y = {{{0, 96}, {255, 96}}};
    p.chroma_scaling_from_luma = 0;
    p.num_cb_points = 2;
    p.scaling_points_cb = {{{0, 64}, {255, 64}}};
    p.num_cr_points = 2;
    p.scaling_points_cr = {{{0, 64}, {255, 64}}};
    p.ar_coeff_lag = 3;
    p.ar_coeffs_y = {4, 1, 3, 0, 1, -3, 8, -3, 7, -23, 1, -25,
                     0, -10, 6, -17, -4, 53, 36, 5, -5, -17, 8, 66};
    p.ar_coeffs_cb = {0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0, 127};
    p.ar_coeffs_cr = {0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0, 127};
    p.ar_coeff_shift = 1;   // actual = 6+1 = 7
    p.cb_mult = 128; p.cb_luma_mult = 192; p.cb_offset = 256;
    p.cr_mult = 128; p.cr_luma_mult = 192; p.cr_offset = 256;
    p.scaling_shift = 4;    // actual = 7+4 = 11
    p.overlap_flag = 1;
    p.clip_to_restricted_range = 0;
    p.bit_depth = 8;
    p.grain_scale_shift = 0;
    p.random_seed = 45231;
    return p;
}

// AOM Test 3: lag=3, grain_scale_shift=1, clip_to_restricted_range=1
inline Av1FilmGrainSynthesisData aomVector3() {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 1;
    p.update_parameters = 1;
    p.num_y_points = 2;
    p.scaling_points_y = {{{0, 192}, {255, 192}}};
    p.chroma_scaling_from_luma = 0;
    p.num_cb_points = 2;
    p.scaling_points_cb = {{{0, 128}, {255, 128}}};
    p.num_cr_points = 2;
    p.scaling_points_cr = {{{0, 128}, {255, 128}}};
    p.ar_coeff_lag = 3;
    p.ar_coeffs_y = {4, 1, 3, 0, 1, -3, 8, -3, 7, -23, 1, -25,
                     0, -10, 6, -17, -4, 53, 36, 5, -5, -17, 8, 66};
    p.ar_coeffs_cb = {4, -7, 2, 4, 12, -12, 5, -8, 6, 8, -19, -16, 19,
                      -10, -2, 17, -42, 58, -2, -13, 9, 14, -36, 67, 0};
    p.ar_coeffs_cr = {4, -7, 2, 4, 12, -12, 5, -8, 6, 8, -19, -16, 19,
                      -10, -2, 17, -42, 58, -2, -13, 9, 14, -36, 67, 0};
    p.ar_coeff_shift = 1;   // actual = 7
    p.cb_mult = 128; p.cb_luma_mult = 192; p.cb_offset = 256;
    p.cr_mult = 128; p.cr_luma_mult = 192; p.cr_offset = 256;
    p.scaling_shift = 4;    // actual = 11
    p.overlap_flag = 1;
    p.clip_to_restricted_range = 1;
    p.bit_depth = 8;
    p.grain_scale_shift = 1;
    p.random_seed = 45231;
    return p;
}

// AOM Test 6: Luma only (zero chroma points), 14-point ramp-down curve
inline Av1FilmGrainSynthesisData aomVector6LumaOnly() {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 1;
    p.update_parameters = 1;
    p.num_y_points = 14;
    p.scaling_points_y = {{
        {0,96}, {20,92}, {39,88}, {59,84}, {78,80}, {98,75}, {118,70},
        {137,65}, {157,60}, {177,53}, {196,46}, {216,38}, {235,27}, {255,0}
    }};
    p.chroma_scaling_from_luma = 0;
    p.num_cb_points = 0;
    p.num_cr_points = 0;
    p.ar_coeff_lag = 3;
    p.ar_coeffs_y = {4, 1, 3, 0, 1, -3, 8, -3, 7, -23, 1, -25,
                     0, -10, 6, -17, -4, 53, 36, 5, -5, -17, 8, 66};
    p.ar_coeff_shift = 1;   // actual = 7
    p.scaling_shift = 4;    // actual = 11
    p.overlap_flag = 1;
    p.clip_to_restricted_range = 0;
    p.bit_depth = 8;
    p.grain_scale_shift = 0;
    p.random_seed = 2754;
    return p;
}

// [MMSJ] Custom: chroma_scaling_from_luma=1
inline Av1FilmGrainSynthesisData chromaFromLumaVector() {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 1;
    p.update_parameters = 1;
    p.num_y_points = 2;
    p.scaling_points_y = {{{0, 80}, {255, 80}}};
    p.chroma_scaling_from_luma = 1;
    p.num_cb_points = 0;
    p.num_cr_points = 0;
    p.ar_coeff_lag = 2;
    p.ar_coeffs_y = {0, 0, -58, 0, 0, 0, -76, 100, -43, 0, -51, 82};
    p.ar_coeffs_cb = {0, 0, -30, 0, 0, 0, -20, 15, -10, 0, -15, 5, 40};
    p.ar_coeffs_cr = {0, 0, -25, 0, 0, 0, -18, 12, -8, 0, -12, 8, 35};
    p.ar_coeff_shift = 2;   // actual = 8
    p.cb_mult = 128; p.cb_luma_mult = 192; p.cb_offset = 256;
    p.cr_mult = 128; p.cr_luma_mult = 192; p.cr_offset = 256;
    p.scaling_shift = 4;    // actual = 11
    p.overlap_flag = 0;
    p.clip_to_restricted_range = 0;
    p.bit_depth = 8;
    p.grain_scale_shift = 0;
    p.random_seed = 9999;
    return p;
}

}  // namespace av1::fg::test
