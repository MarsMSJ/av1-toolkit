#include "film_grain.hpp"

namespace av1 {

using namespace av1::fg;

int FilmGrainSynthesizer::synthesize(const Av1FilmGrainSynthesisData &fg_params,
                                     IntXY chroma_subsampling) {
  if (!fg_params.apply_grain)
    return 0;

  // Extract commonly used params into named locals
  const int ar_coeff_lag  = fg_params.ar_coeff_lag;
  const int num_y_points  = fg_params.num_y_points;
  const int bit_depth     = fg_params.bit_depth;

  // Grain sample range for this bit depth (8-bit: [-128, 127])
  const int grain_center = 128 << (bit_depth - 8);
  const int grain_min    = -grain_center;
  const int grain_max    = grain_center - 1;

  // Chroma subblock sizes (useful for HW decoder tiling)
  const int chr_sblk_sz_x = kAomConstants::kLumaSubBlockSz.x >> chroma_subsampling.x;
  const int chr_sblk_sz_y = kAomConstants::kLumaSubBlockSz.y >> chroma_subsampling.y;

  // Compute block sizes
  const auto [luma_block_size_x, luma_block_size_y]     = kAomConstants::getLumaBlockSize();
  const auto [chroma_block_size_x, chroma_block_size_y] = kAomConstants::getChromaBlockSize(chroma_subsampling);

  const int luma_grain_stride   = luma_block_size_x;
  const int chroma_grain_stride = chroma_block_size_x;

  // AR prediction position tables
  const int num_pos_luma   = 2 * ar_coeff_lag * (ar_coeff_lag + 1);
  const int num_pos_chroma = num_pos_luma + (num_y_points > 0 ? 1 : 0);

  PredictionPositionTable luma_pred_tbl{};
  PredictionPositionTable chroma_pred_tbl{};
  kAomConstants::buildPositionPredictionTables(ar_coeff_lag, num_y_points,
                                               luma_pred_tbl, chroma_pred_tbl);

  // --- Scaling LUTs ---
  synthesizeScalingLutY(fg_params, out_.scaling_lut_y);

  if (fg_params.chroma_scaling_from_luma) {
    out_.scaling_lut_cb = out_.scaling_lut_y;
    out_.scaling_lut_cr = out_.scaling_lut_y;
  } else {
    synthesizeScalingLutCb(fg_params, out_.scaling_lut_cb, out_.scaling_lut_y);
    synthesizeScalingLutCr(fg_params, out_.scaling_lut_cr, out_.scaling_lut_y);
  }

  // --- Grain template generation ---
  out_.luma_grain.fill(0);
  out_.cb_grain.fill(0);
  out_.cr_grain.fill(0);

  Av1FgPrng prng(fg_params.random_seed);

  generateLumaGrainBlock(fg_params, luma_pred_tbl, num_pos_luma, prng,
                         out_.luma_grain.data(),
                         luma_block_size_y, luma_block_size_x,
                         luma_grain_stride,
                         grain_min, grain_max);

  generateChromaGrainBlocks(fg_params, chroma_pred_tbl, num_pos_chroma, prng,
                            out_.luma_grain.data(),
                            out_.cb_grain.data(), out_.cr_grain.data(),
                            luma_grain_stride,
                            chroma_block_size_y, chroma_block_size_x,
                            chroma_grain_stride,
                            chroma_subsampling.y, chroma_subsampling.x,
                            grain_min, grain_max);

  // Store template dimensions
  out_.luma_block_size_y   = luma_block_size_y;
  out_.luma_block_size_x   = luma_block_size_x;
  out_.chroma_block_size_y = chroma_block_size_y;
  out_.chroma_block_size_x = chroma_block_size_x;

  AV1_FG_DBG("synthesize: seed=%u lag=%d bit_depth=%d chr_sblk=%dx%d "
             "luma=%dx%d chroma=%dx%d",
             fg_params.random_seed, ar_coeff_lag, bit_depth,
             chr_sblk_sz_x, chr_sblk_sz_y,
             luma_block_size_y, luma_block_size_x,
             chroma_block_size_y, chroma_block_size_x);

  return 0;
}

} // namespace av1
