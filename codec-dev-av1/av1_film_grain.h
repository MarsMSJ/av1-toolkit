#pragma once
#include "sdec_av1_core.h"
#include <algorithm>

namespace sce::vdec::av1::fg {
#include "../av1_fg_prng.h"
#include "../av1_fg_aom_const.h"
#include "../av1_fg_common.h"

extern const int16_t av1_gaussian_sequence[2048];

void getFilmGraintDataParams(Sdec::Av1FilmGrainSegmentDataParams &ref_seg_data_params, Sdec::Av1FilmGrain &fg_data);

void synthesizeScalingLutY(Sdec::Av1FilmGrainSegmentDataParams &fg_seg_params, Sdec::Av1FilmGrain &fg_data);

void synthesizeScalingLutCb(Sdec::Av1FilmGrainSegmentDataParams &fg_seg_params, Sdec::Av1FilmGrain &fg_data);

void synthesizeScalingLutCr(Sdec::Av1FilmGrainSegmentDataParams &fg_seg_params, Sdec::Av1FilmGrain &fg_data);

void generateFilmGrainBlocks(Sdec::Av1FilmGrainSegmentDataParams &ref_seg_data_params, Sdec::Av1FilmGrain &fg_data);

class FilmGrainSynthesizer {

  public:
    int synthesize(const FilmGrainParams &params, IntXY chroma_subsamp, FgSynthesisOutput &out);

    FilmGrainSynthesizer();
    ~FilmGrainSynthesizer();
};

}  // namespace sce::vdec::av1::fg