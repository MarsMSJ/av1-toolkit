#pragma once


#include <cstdint>
#include "av1_fg_common.h"

namespace av1::fg {

// [MMSJ] AOM: av1/decoder/grain_synthesis.c
struct kAomConstants
{
    static constexpr size_t kFP16Scale      = 1 << 16;  // [MMSJ] AOM fixed-point 16.16
    static constexpr IntXY  kLumaSubBlockSz = {32, 32}; // [MMSJ] §7.18.3.5
    static constexpr int    left_pad        = 3;        // [MMSJ] AOM: max lag=3
    static constexpr int    right_pad       = 3;
    static constexpr int    top_pad         = 3;
    static constexpr int    bottom_pad      = 0;
    static constexpr int    ar_padding      = 3;

    // Luma block: 82 x 73 = 5986 samples (deterministic, independent of subsampling)
    static constexpr int kLumaBlockX = left_pad + 2 * ar_padding + kLumaSubBlockSz.x * 2 + 2 * ar_padding + right_pad;
    static constexpr int kLumaBlockY = top_pad  + 2 * ar_padding + kLumaSubBlockSz.y * 2 + bottom_pad;
    static constexpr int kLumaGrainSamples = kLumaBlockX * kLumaBlockY;

    // Max chroma block is same as luma (4:4:4, subsamp=0,0)
    static constexpr int kMaxChromaGrainSamples = kLumaGrainSamples;

    static IntXY getLumaBlockSize()
    {
        auto x = left_pad + 2 * ar_padding + kLumaSubBlockSz.x * 2 + 2 * ar_padding + right_pad;
        auto y = top_pad + 2 * ar_padding + kLumaSubBlockSz.y * 2 + bottom_pad;
        return {x, y};
    }

    static IntXY getChromaBlockSize(IntXY chr_sub_samp)
    {
        const int px = (2 >> chr_sub_samp.x) * ar_padding;
        const int py = (2 >> chr_sub_samp.y) * ar_padding;
        auto x = left_pad + px + (kLumaSubBlockSz.x >> chr_sub_samp.x) * 2 + px + right_pad;
        auto y = top_pad + py + (kLumaSubBlockSz.y >> chr_sub_samp.y) * 2 + bottom_pad;
        return {x, y};
    }

    // [MMSJ] §7.18.3.4
    static void buildPositionPredictionTables(int ar_coeff_lag, int num_y_points, PredictionPositionTable &luma_tbl, PredictionPositionTable &chr_tbl)
    {
        int pos = 0;
        for (int row = -ar_coeff_lag; row < 0; row++)
        {
            for (int col = -ar_coeff_lag; col <= ar_coeff_lag; col++)
            {
                luma_tbl[pos] = {row, col, 0};
                chr_tbl[pos]  = {row, col, 0};
                ++pos;
            }
        }
        for (int col = -ar_coeff_lag; col < 0; col++)
        {
            luma_tbl[pos] = {0, col, 0};
            chr_tbl[pos]  = {0, col, 0};
            ++pos;
        }

        //
        if (num_y_points > 0)
        {
            chr_tbl[pos] = {0, 0, 1};
        }
    }
};

}  // namespace av1::fg