#pragma once

// [MMSJ] §7.18.3.2 — grain template generation / AOM: av1/decoder/grain_synthesis.c

#include <algorithm>
#include <cstring>
#include "av1_fg_common.h"
#include "av1_fg_aom_const.h"
#include "av1_fg_gaussian.h"
#include "fg_prng.h"

namespace av1::fg {

namespace detail {

inline int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

} // namespace detail

// [MMSJ] §7.18.3.2 generate_luma_grain_block
inline void generateLumaGrainBlock(
    const Av1FilmGrainSynthesisData& params,
    const PredictionPositionTable& pred_pos_luma,
    int num_pos_luma,
    Av1FgPrng& prng,
    int* luma_grain_block,
    int luma_block_size_y, int luma_block_size_x, int luma_grain_stride,
    int grain_min, int grain_max)
{
    if (params.num_y_points == 0) {
        std::fill(luma_grain_block,
                  luma_grain_block + luma_block_size_y * luma_grain_stride, 0);
        return;
    }

    const int gauss_sec_shift = 12 - params.bit_depth + params.grain_scale_shift;
    const int rounding_offset = (1 << (params.ar_coeff_shift - 1));

    // Fill entire block with scaled Gaussian samples
    for (int i = 0; i < luma_block_size_y; i++)
        for (int j = 0; j < luma_block_size_x; j++)
            luma_grain_block[i * luma_grain_stride + j] =
                (kGaussianSequence[prng.get(kGaussBits)] +
                 ((1 << gauss_sec_shift) >> 1)) >>
                gauss_sec_shift;

    // AR filter: each valid sample is updated using its causal neighbourhood.
    // Padding rows/cols guarantee all reference positions are in bounds.
    for (int i = kAomConstants::top_pad; i < luma_block_size_y - kAomConstants::bottom_pad; i++) {
        for (int j = kAomConstants::left_pad; j < luma_block_size_x - kAomConstants::right_pad; j++) {
            int wsum = 0;
            for (int pos = 0; pos < num_pos_luma; pos++) {
                wsum += params.ar_coeffs_y[pos] *
                        luma_grain_block[(i + pred_pos_luma[pos][0]) * luma_grain_stride +
                                         j + pred_pos_luma[pos][1]];
            }
            luma_grain_block[i * luma_grain_stride + j] =
                detail::clamp(luma_grain_block[i * luma_grain_stride + j] +
                              ((wsum + rounding_offset) >> params.ar_coeff_shift),
                              grain_min, grain_max);
        }
    }
}

// [MMSJ] §7.18.3.2 generate_chroma_grain_blocks
inline void generateChromaGrainBlocks(
    const Av1FilmGrainSynthesisData& params,
    const PredictionPositionTable& pred_pos_chroma,
    int num_pos_chroma,
    Av1FgPrng& prng,
    const int* luma_grain_block,
    int* cb_grain_block, int* cr_grain_block,
    int luma_grain_stride,
    int chroma_block_size_y, int chroma_block_size_x, int chroma_grain_stride,
    int chroma_subsamp_y, int chroma_subsamp_x,
    int grain_min, int grain_max)
{
    const int gauss_sec_shift = 12 - params.bit_depth + params.grain_scale_shift;
    const int rounding_offset = (1 << (params.ar_coeff_shift - 1));
    const int chroma_grain_block_size = chroma_block_size_y * chroma_grain_stride;

    // Cb — separate PRNG seed 7<<5 per spec §7.18.3.2
    if (params.num_cb_points > 0 || params.chroma_scaling_from_luma) {
        prng.init(Av1FgPrng::kCbStripeIdx, params.random_seed);
        for (int i = 0; i < chroma_block_size_y; i++)
            for (int j = 0; j < chroma_block_size_x; j++)
                cb_grain_block[i * chroma_grain_stride + j] =
                    (kGaussianSequence[prng.get(kGaussBits)] +
                     ((1 << gauss_sec_shift) >> 1)) >>
                    gauss_sec_shift;
    } else {
        std::fill(cb_grain_block, cb_grain_block + chroma_grain_block_size, 0);
    }

    // Cr — separate PRNG seed 11<<5
    if (params.num_cr_points > 0 || params.chroma_scaling_from_luma) {
        prng.init(Av1FgPrng::kCrStripeIdx, params.random_seed);
        for (int i = 0; i < chroma_block_size_y; i++)
            for (int j = 0; j < chroma_block_size_x; j++)
                cr_grain_block[i * chroma_grain_stride + j] =
                    (kGaussianSequence[prng.get(kGaussBits)] +
                     ((1 << gauss_sec_shift) >> 1)) >>
                    gauss_sec_shift;
    } else {
        std::fill(cr_grain_block, cr_grain_block + chroma_grain_block_size, 0);
    }

    // AR filter for both chroma planes simultaneously
    const int top_pad    = kAomConstants::top_pad;
    const int left_pad   = kAomConstants::left_pad;
    const int bottom_pad = kAomConstants::bottom_pad;
    const int right_pad  = kAomConstants::right_pad;

    for (int i = top_pad; i < chroma_block_size_y - bottom_pad; i++) {
        for (int j = left_pad; j < chroma_block_size_x - right_pad; j++) {
            int wsum_cb = 0;
            int wsum_cr = 0;
            for (int pos = 0; pos < num_pos_chroma; pos++) {
                if (pred_pos_chroma[pos][2] == 0) {
                    // Autoregressive term from chroma neighbours
                    wsum_cb += params.ar_coeffs_cb[pos] *
                               cb_grain_block[(i + pred_pos_chroma[pos][0]) *
                                                  chroma_grain_stride +
                                              j + pred_pos_chroma[pos][1]];
                    wsum_cr += params.ar_coeffs_cr[pos] *
                               cr_grain_block[(i + pred_pos_chroma[pos][0]) *
                                                  chroma_grain_stride +
                                              j + pred_pos_chroma[pos][1]];
                } else {
                    // Luma coupling: average the corresponding luma grain samples.
                    // For 4:2:0 each chroma sample maps to a 2x2 luma region.
                    int luma_coord_y = ((i - top_pad) << chroma_subsamp_y) + top_pad;
                    int luma_coord_x = ((j - left_pad) << chroma_subsamp_x) + left_pad;
                    int av_luma = 0;
                    for (int k = luma_coord_y; k < luma_coord_y + chroma_subsamp_y + 1; k++)
                        for (int l = luma_coord_x; l < luma_coord_x + chroma_subsamp_x + 1; l++)
                            av_luma += luma_grain_block[k * luma_grain_stride + l];
                    av_luma = (av_luma + ((1 << (chroma_subsamp_y + chroma_subsamp_x)) >> 1)) >>
                              (chroma_subsamp_y + chroma_subsamp_x);
                    wsum_cb += params.ar_coeffs_cb[pos] * av_luma;
                    wsum_cr += params.ar_coeffs_cr[pos] * av_luma;
                }
            }
            if (params.num_cb_points > 0 || params.chroma_scaling_from_luma)
                cb_grain_block[i * chroma_grain_stride + j] =
                    detail::clamp(cb_grain_block[i * chroma_grain_stride + j] +
                                  ((wsum_cb + rounding_offset) >> params.ar_coeff_shift),
                                  grain_min, grain_max);
            if (params.num_cr_points > 0 || params.chroma_scaling_from_luma)
                cr_grain_block[i * chroma_grain_stride + j] =
                    detail::clamp(cr_grain_block[i * chroma_grain_stride + j] +
                                  ((wsum_cr + rounding_offset) >> params.ar_coeff_shift),
                                  grain_min, grain_max);
        }
    }
}

}  // namespace av1::fg