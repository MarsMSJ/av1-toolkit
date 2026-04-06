#pragma once

// [MMSJ] §7.18.3.2, §7.18.3.3 — scaling LUT / AOM: av1/decoder/grain_synthesis.c

#include <array>
#include <cstdint>
#include "av1_fg_common.h"
#include "av1_fg_aom_const.h"

namespace av1::fg {

// [MMSJ] §7.18.3.2 init_scaling_function
inline void initScalingFunction(const std::array<int, 2>* pts, int num_points,
                                std::array<int, 256>& lut) {
    if (num_points == 0)
        return;

    for (int i = 0; i < pts[0][0]; i++)
        lut[i] = pts[0][1];

    for (int p = 0; p < num_points - 1; p++) {
        int delta_y = pts[p + 1][1] - pts[p][1];
        int delta_x = pts[p + 1][0] - pts[p][0];
        int64_t delta = delta_y * ((static_cast<int64_t>(kAomConstants::kFP16Scale) + (delta_x >> 1)) / delta_x);
        for (int x = 0; x < delta_x; x++)
            lut[pts[p][0] + x] = pts[p][1] + static_cast<int>((x * delta + 32768) >> 16);
    }

    for (int i = pts[num_points - 1][0]; i < 256; i++)
        lut[i] = pts[num_points - 1][1];
}

// [MMSJ] §7.18.3.3 scale_LUT
inline int scaleLUT(const std::array<int, 256>& scaling_lut, int index, int bit_depth) {
    int x = index >> (bit_depth - 8);
    if (!(bit_depth - 8) || x == 255)
        return scaling_lut[x];
    return scaling_lut[x] + (((scaling_lut[x + 1] - scaling_lut[x]) *
                                  (index & ((1 << (bit_depth - 8)) - 1)) +
                              (1 << (bit_depth - 9))) >>
                             (bit_depth - 8));
}

// Per-plane scaling LUT synthesis helpers
inline void synthesizeScalingLutY(const Av1FilmGrainSynthesisData& params,
                                  std::array<int, 256>& lut) {
    initScalingFunction(params.scaling_points_y.data(), params.num_y_points, lut);
}

inline void synthesizeScalingLutCb(const Av1FilmGrainSynthesisData& params,
                                   std::array<int, 256>& lut,
                                   const std::array<int, 256>& lut_y) {
    if (params.chroma_scaling_from_luma)
        lut = lut_y;
    else
        initScalingFunction(params.scaling_points_cb.data(), params.num_cb_points, lut);
}

inline void synthesizeScalingLutCr(const Av1FilmGrainSynthesisData& params,
                                   std::array<int, 256>& lut,
                                   const std::array<int, 256>& lut_y) {
    if (params.chroma_scaling_from_luma)
        lut = lut_y;
    else
        initScalingFunction(params.scaling_points_cr.data(), params.num_cr_points, lut);
}

}  // namespace av1::fg