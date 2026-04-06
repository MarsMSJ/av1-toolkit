#pragma once

// AV1 Film Grain Synthesis — orchestrator header
//
// Includes the modular components and provides the top-level
// FilmGrainSynthesizer class that produces scaling LUTs + grain templates.
// Pixel application (tiling, overlap, noise addition) is the HW decoder's job.

#include <array>
#include <cstdint>

#include "av1_fg_common.h"
#include "av1_fg_aom_const.h"
#include "av1_fg_gaussian.h"
#include "av1_fg_scaling.h"
#include "av1_fg_grain_gen.h"
#include "av1_fg_dbg.h"
#include "fg_prng.h"

namespace av1 {

using namespace av1::fg;

// Synthesis output — scaling LUTs + grain templates.
// HW decoder receives the Av1FilmGrainSynthesisData struct directly; this
// carries the pre-computed synthesis artefacts for validation or SW fallback.
struct FgSynthesisOutput {
    // Scaling LUTs (Section 7.18.3.2) — 256 entries, indexed by 8-bit pixel value.
    // For 10-bit, scaleLUT() interpolates between adjacent entries at lookup time.
    // chroma LUTs == luma LUT when chroma_scaling_from_luma is set.
    std::array<int, 256> scaling_lut_y{};
    std::array<int, 256> scaling_lut_cb{};
    std::array<int, 256> scaling_lut_cr{};

    // Grain templates — flat row-major, stride == block_size_x
    // Sized at max (luma block = 82x73); chroma may use less depending on subsampling.
    std::array<int, kAomConstants::kLumaGrainSamples>     luma_grain{};
    std::array<int, kAomConstants::kMaxChromaGrainSamples> cb_grain{};
    std::array<int, kAomConstants::kMaxChromaGrainSamples> cr_grain{};

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
    // Returns 0 on success.
    int synthesize(const Av1FilmGrainSynthesisData& fg_params,
                                IntXY chroma_subsampling);

    // Access the synthesis results after calling synthesize().
    const FgSynthesisOutput& output() const { return out_; }

private:
    FgSynthesisOutput out_{};
};

} // namespace av1
