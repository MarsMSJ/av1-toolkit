/*
 * SIE CONFIDENTIAL
 * $PSLibId$
 * Copyright (C) 2026 Sony Interactive Entertainment Inc.
 * All Rights Reserved.
 *
 * File:        av1_fg_aom_const.h
 * Module:      AV1 Internal Decoder - AOM Constants
 * Author:      Mario Sarriá Jr.
 * Date:        2026-04-03
 * Version:     1.0
 * Email:       mario.sarriajr@sony.com
 * Description:
 *   AOM constants for AV1 film grain synthesis based on AV1 specification section 7.18.3
 */
#pragma once
#include <cstdint>
#include "av1_fg_common.h"

struct kAomConstants
{
    // clang-format off
    static constexpr size_t kFP16Scale      = 1 << 16;              // Precision of scale is directly from AOM reference implementation.
    static constexpr IntXY  kLumaSubBlockSz = {32, 32};       // §7.18.3.5 generates grain in 32-sample sub-blocks tiled to 64×64
    static constexpr int    left_pad        = 3;                    // Spec-derived padding for AR filter; hardcoded to the max lag value
    static constexpr int    right_pad       = 3;                    // Spec-derived padding for AR filter; hardcoded to the max lag value
    static constexpr int    top_pad         = 3;                    // Spec-derived padding for AR filter; hardcoded to the max lag value
    static constexpr int    bottom_pad      = 0;                    // No lookahead needed; taken directly from AOM reference implementation.
    static constexpr int    ar_padding      = 3;                    // Spec-derived padding for AR filter; hardcoded to the max lag value
    // clang-format on

    static constexpr IntXY getChromaSubBlockSize(IntXY chroma_subsamp)
    {
        return {kLumaSubBlockSz.x >> chroma_subsamp.x, kLumaSubBlockSz.y >> chroma_subsamp.y};
    }

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
        auto      x  = left_pad + px + (kLumaSubBlockSz.x >> chr_sub_samp.x) * 2 + px + right_pad;
        auto      y  = top_pad + py + (kLumaSubBlockSz.y >> chr_sub_samp.y) * 2 + bottom_pad;
        return {x, y};
    }

    static void buildPositionPredictionTables(int                      ar_coeff_lag,
                                              int                      num_y_points,
                                              PredictionPositionTable &luma_tbl,
                                              PredictionPositionTable &chr_tbl)
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