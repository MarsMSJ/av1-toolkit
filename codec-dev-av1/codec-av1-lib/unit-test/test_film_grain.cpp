#include <gtest/gtest.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include "../film_grain.hpp"

using namespace av1;

// ============================================================================
// Helpers
// ============================================================================

// Minimal valid params for 8-bit 4:2:0, no AR filter, no grain yet.
// Individual tests layer on the grain points they need.
static FilmGrainParams make_base_params() {
    FilmGrainParams p;
    p.apply_grain       = true;
    p.bit_depth         = 8;
    p.scaling_shift     = 8;       // must be 8..11 per spec
    p.ar_coeff_lag      = 0;       // no AR filter in the base case
    p.ar_coeff_shift    = 6;       // must be 6..9 per spec
    p.grain_scale_shift = 0;
    p.random_seed       = 7391;
    p.overlap_flag      = false;
    p.clip_to_restricted_range  = false;
    p.chroma_scaling_from_luma  = false;
    // Neutral chroma mix: no luma/chroma coupling offset
    p.cb_mult = 128; p.cb_luma_mult = 192; p.cb_offset = 256;
    p.cr_mult = 128; p.cr_luma_mult = 192; p.cr_offset = 256;
    return p;
}

// Constant scaling curve: LUT outputs `scale` for every pixel value.
// With scaling_shift=8 the grain contribution per pixel ≈ (scale * grain) >> 8.
// Using scale=64 and grain range [-128,127] gives contributions in [-32, 31].
static void add_y_scaling(FilmGrainParams& p, int scale = 64) {
    p.num_y_points = 2;
    p.scaling_points_y[0] = {0,   scale};
    p.scaling_points_y[1] = {255, scale};
}
static void add_cb_scaling(FilmGrainParams& p, int scale = 64) {
    p.num_cb_points = 2;
    p.scaling_points_cb[0] = {0,   scale};
    p.scaling_points_cb[1] = {255, scale};
}
static void add_cr_scaling(FilmGrainParams& p, int scale = 64) {
    p.num_cr_points = 2;
    p.scaling_points_cr[0] = {0,   scale};
    p.scaling_points_cr[1] = {255, scale};
}

// A self-contained YCbCr frame + destination buffer, 4:2:0.
struct Frame420 {
    int w, h;
    int uv_w, uv_h;

    std::vector<uint8_t> y,  cb,  cr;   // source
    std::vector<uint8_t> dy, dcb, dcr;  // destination

    Frame420(int width, int height, uint8_t fill = 128)
        : w(width), h(height),
          uv_w(width >> 1), uv_h(height >> 1),
          y  (width * height,     fill),
          cb (uv_w   * uv_h,      fill),
          cr (uv_w   * uv_h,      fill),
          dy (width * height,     0),
          dcb(uv_w   * uv_h,      0),
          dcr(uv_w   * uv_h,      0) {}

    int run(FilmGrainSynthesizer& s, const FilmGrainParams& p) {
        return s.add_film_grain(p,
            y,  w,    cb, uv_w, cr, uv_w,
            dy, w,   dcb, uv_w, dcr, uv_w,
            w, h, /*chroma_subsamp_x=*/1, /*chroma_subsamp_y=*/1);
    }

    bool dst_all_in_range(int lo, int hi) const {
        for (auto v : dy)  if (v < lo || v > hi) return false;
        for (auto v : dcb) if (v < lo || v > hi) return false;
        for (auto v : dcr) if (v < lo || v > hi) return false;
        return true;
    }

    // Returns true if at least one destination pixel differs from the source.
    bool grain_was_applied() const {
        return dy != y || dcb != cb || dcr != cr;
    }
};

// ============================================================================
// Existing baseline tests (behaviour unchanged)
// ============================================================================

TEST(FilmGrainTest, CheckGrainParamsEquiv) {
    FilmGrainParams pa, pb;

    EXPECT_TRUE(check_grain_params_equiv(pa, pb));

    pa.apply_grain = true;
    EXPECT_FALSE(check_grain_params_equiv(pa, pb));

    pb.apply_grain = true;
    EXPECT_TRUE(check_grain_params_equiv(pa, pb));

    // update_parameters is intentionally excluded from the equivalence check
    pa.update_parameters = true;
    EXPECT_TRUE(check_grain_params_equiv(pa, pb));
}

TEST(FilmGrainTest, AddFilmGrainCopySuccess) {
    FilmGrainSynthesizer synthesizer;
    FilmGrainParams params;
    params.apply_grain = false;

    const int width = 16, height = 16, stride = 16;
    std::vector<uint8_t> src_y (width * height,   128);
    std::vector<uint8_t> src_cb(width * height / 4, 128);
    std::vector<uint8_t> src_cr(width * height / 4, 128);
    std::vector<uint8_t> dst_y (width * height,   0);
    std::vector<uint8_t> dst_cb(width * height / 4, 0);
    std::vector<uint8_t> dst_cr(width * height / 4, 0);

    int result = synthesizer.add_film_grain(params,
        src_y,  stride, src_cb, stride / 2, src_cr, stride / 2,
        dst_y,  stride, dst_cb, stride / 2, dst_cr, stride / 2,
        width, height, 1, 1);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(dst_y,  src_y);
    EXPECT_EQ(dst_cb, src_cb);
    EXPECT_EQ(dst_cr, src_cr);
}

TEST(FilmGrainTest, AddFilmGrainErrors) {
    FilmGrainSynthesizer synthesizer;
    FilmGrainParams params;

    const int width = 16, height = 16;
    std::vector<uint8_t> src_y (width * height,     128);
    std::vector<uint8_t> src_cb(width * height / 4, 128);
    std::vector<uint8_t> src_cr(width * height / 4, 128);
    std::vector<uint8_t> dst_y (width * height,     0);
    std::vector<uint8_t> dst_cb(width * height / 4, 0);
    std::vector<uint8_t> dst_cr(width * height / 4, 0);

    std::vector<uint8_t> empty;
    EXPECT_EQ(synthesizer.add_film_grain(params,
        empty, width, src_cb, width/2, src_cr, width/2,
        dst_y, width, dst_cb, width/2, dst_cr, width/2,
        width, height, 1, 1), -1);

    std::vector<uint8_t> too_small(3, 0);
    EXPECT_EQ(synthesizer.add_film_grain(params,
        src_y, width, too_small, width/2, src_cr, width/2,
        dst_y, width, dst_cb,    width/2, dst_cr, width/2,
        width, height, 1, 1), -1);
}

// ============================================================================
// Synthesis tests
// ============================================================================

// --- Grain is actually added when apply_grain is true -----------------------

TEST(FilmGrainSynthesis, LumaGrainApplied) {
    // With luma scaling points set, the luma plane must change.
    // Chroma points are absent so chroma passes through unchanged.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_NE(f.dy,  f.y)   << "luma grain should have been applied";
    EXPECT_EQ(f.dcb, f.cb)  << "chroma cb should be unchanged (no cb points)";
    EXPECT_EQ(f.dcr, f.cr)  << "chroma cr should be unchanged (no cr points)";
}

TEST(FilmGrainSynthesis, ChromaGrainApplied) {
    // With only chroma points set and no luma points, chroma changes but luma
    // passes through.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_EQ(f.dy,  f.y)   << "luma should be unchanged (no y points)";
    EXPECT_NE(f.dcb, f.cb)  << "cb grain should have been applied";
    EXPECT_NE(f.dcr, f.cr)  << "cr grain should have been applied";
}

TEST(FilmGrainSynthesis, AllPlanesGrainApplied) {
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
}

// --- Output always stays within the legal 8-bit range ----------------------

TEST(FilmGrainSynthesis, OutputPixelRange) {
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_y_scaling(p, 255); // maximum scale to stress-test clamping
    add_cb_scaling(p, 255);
    add_cr_scaling(p, 255);

    // Test with pixels at extremes (0 and 255) as well as mid-range.
    for (uint8_t fill : {0, 1, 128, 254, 255}) {
        Frame420 f(64, 64, fill);
        ASSERT_EQ(f.run(s, p), 0);
        EXPECT_TRUE(f.dst_all_in_range(0, 255))
            << "pixel out of [0,255] with fill=" << (int)fill;
    }
}

TEST(FilmGrainSynthesis, ClipToRestrictedRange) {
    // With clip_to_restricted_range, luma must stay in [16,235] and
    // chroma in [16,240].
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.clip_to_restricted_range = true;
    add_y_scaling(p, 255);
    add_cb_scaling(p, 255);
    add_cr_scaling(p, 255);

    for (uint8_t fill : {0, 16, 128, 235, 255}) {
        Frame420 f(64, 64, fill);
        ASSERT_EQ(f.run(s, p), 0);

        for (auto v : f.dy)
            EXPECT_GE(v, 16) << "luma below restricted minimum";
        for (auto v : f.dy)
            EXPECT_LE(v, 235) << "luma above restricted maximum";

        for (auto v : f.dcb)
            EXPECT_GE(v, 16) << "cb below restricted minimum";
        for (auto v : f.dcb)
            EXPECT_LE(v, 240) << "cb above restricted maximum";

        for (auto v : f.dcr)
            EXPECT_GE(v, 16) << "cr below restricted minimum";
        for (auto v : f.dcr)
            EXPECT_LE(v, 240) << "cr above restricted maximum";
    }
}

// --- PRNG / reproducibility -------------------------------------------------

TEST(FilmGrainSynthesis, SameSeedProducesSameOutput) {
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f1(64, 64);
    Frame420 f2(64, 64);

    FilmGrainSynthesizer s1, s2;
    ASSERT_EQ(f1.run(s1, p), 0);
    ASSERT_EQ(f2.run(s2, p), 0);

    EXPECT_EQ(f1.dy,  f2.dy)  << "same seed must produce identical luma output";
    EXPECT_EQ(f1.dcb, f2.dcb) << "same seed must produce identical cb output";
    EXPECT_EQ(f1.dcr, f2.dcr) << "same seed must produce identical cr output";
}

TEST(FilmGrainSynthesis, DifferentSeedsProduceDifferentOutput) {
    FilmGrainParams p1 = make_base_params();
    FilmGrainParams p2 = make_base_params();
    p2.random_seed = p1.random_seed + 1;

    add_y_scaling(p1); add_cb_scaling(p1); add_cr_scaling(p1);
    add_y_scaling(p2); add_cb_scaling(p2); add_cr_scaling(p2);

    Frame420 f1(64, 64);
    Frame420 f2(64, 64);

    FilmGrainSynthesizer s1, s2;
    ASSERT_EQ(f1.run(s1, p1), 0);
    ASSERT_EQ(f2.run(s2, p2), 0);

    EXPECT_NE(f1.dy, f2.dy) << "different seeds should produce different grain";
}

// --- AR filter --------------------------------------------------------------

TEST(FilmGrainSynthesis, ARFilterLag1ProducesOutput) {
    // AR filter with lag=1: each grain sample depends on its 3 left/above
    // neighbours.  Just verify it runs correctly and stays in range.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.ar_coeff_lag   = 1;
    p.ar_coeff_shift = 6;
    // Mild AR coefficients (sum < 2^ar_coeff_shift to keep stability)
    p.ar_coeffs_y[0] = 16; p.ar_coeffs_y[1] = 0; p.ar_coeffs_y[2] = -8;
    p.ar_coeffs_cb[0] = 8; p.ar_coeffs_cb[1] = 0; p.ar_coeffs_cb[2] = 0; p.ar_coeffs_cb[3] = 0;
    p.ar_coeffs_cr[0] = 8; p.ar_coeffs_cr[1] = 0; p.ar_coeffs_cr[2] = 0; p.ar_coeffs_cr[3] = 0;
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

TEST(FilmGrainSynthesis, ARFilterLag3ProducesOutput) {
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.ar_coeff_lag   = 3;
    p.ar_coeff_shift = 8;
    // All AR coefficients zero: AR filter has no effect; output must still be
    // in range and grain must be applied via the Gaussian samples alone.
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

TEST(FilmGrainSynthesis, ARFilterChangesOutput) {
    // The same Gaussian base but with non-zero AR coefficients must produce
    // different output than the zero-AR case.
    FilmGrainParams p_no_ar = make_base_params();
    p_no_ar.ar_coeff_lag = 1;
    p_no_ar.ar_coeff_shift = 6;
    add_y_scaling(p_no_ar);

    FilmGrainParams p_ar = p_no_ar;
    p_ar.ar_coeffs_y[0] = 32; // non-zero tap

    Frame420 f1(64, 64), f2(64, 64);
    FilmGrainSynthesizer s1, s2;
    ASSERT_EQ(f1.run(s1, p_no_ar), 0);
    ASSERT_EQ(f2.run(s2, p_ar),    0);

    EXPECT_NE(f1.dy, f2.dy) << "non-zero AR coeffs should change luma output";
}

// --- chroma_scaling_from_luma -----------------------------------------------

TEST(FilmGrainSynthesis, ChromaScalingFromLuma) {
    // When chroma_scaling_from_luma is set, the chroma scaling index is driven
    // by the luma value, not the chroma value.  Output must be in range and
    // chroma must change even with no explicit chroma scaling points.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.chroma_scaling_from_luma = true;
    add_y_scaling(p); // luma points required for chroma-from-luma

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_NE(f.dcb, f.cb) << "cb should change with chroma_scaling_from_luma";
    EXPECT_NE(f.dcr, f.cr) << "cr should change with chroma_scaling_from_luma";
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

// --- overlap_flag -----------------------------------------------------------

TEST(FilmGrainSynthesis, OverlapFlagNoOutOfBounds) {
    // Exercises the border-blending code paths.  Use a frame large enough to
    // have multiple blocks in both dimensions so every overlap branch is hit.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.overlap_flag = true;
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(128, 128);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

TEST(FilmGrainSynthesis, OverlapChangesOutput) {
    // With overlap enabled the boundary pixels must differ from the non-overlap
    // run because the blending weights change those values.
    FilmGrainParams p_no_overlap = make_base_params();
    add_y_scaling(p_no_overlap);

    FilmGrainParams p_overlap = p_no_overlap;
    p_overlap.overlap_flag = true;

    Frame420 f1(128, 128), f2(128, 128);
    FilmGrainSynthesizer s1, s2;
    ASSERT_EQ(f1.run(s1, p_no_overlap), 0);
    ASSERT_EQ(f2.run(s2, p_overlap),    0);

    EXPECT_NE(f1.dy, f2.dy)
        << "overlap_flag should alter grain at block boundaries";
}

// --- Subblock boundary / multi-block ----------------------------------------

TEST(FilmGrainSynthesis, MultiBlockFrame) {
    // 128x128 triggers a 4x4 grid of 32x32 subblocks.
    // Verifies the block-dispatch loop iterates correctly.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(128, 128);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

TEST(FilmGrainSynthesis, SmallFrameSmallerThanOneSubblock) {
    // A 16x16 frame is smaller than the 32x32 subblock size; the clamp inside
    // the block loop must prevent any out-of-bounds writes.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);

    Frame420 f(16, 16);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_TRUE(f.grain_was_applied());
    EXPECT_TRUE(f.dst_all_in_range(0, 255));
}

// --- No scaling points → plane passes through unchanged --------------------

TEST(FilmGrainSynthesis, ZeroScalingPointsMeansNoGrain) {
    // With apply_grain=true but zero scaling points for all planes, the grain
    // blocks for those planes are zeroed out and dst == src.
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    // Leave num_y_points, num_cb_points, num_cr_points all at 0.

    Frame420 f(64, 64);
    ASSERT_EQ(f.run(s, p), 0);

    EXPECT_EQ(f.dy,  f.y);
    EXPECT_EQ(f.dcb, f.cb);
    EXPECT_EQ(f.dcr, f.cr);
}

// --- Scaling LUT: verify interpolation at known points ----------------------

TEST(FilmGrainSynthesis, ScalingLUTBoundaryValues) {
    // Two-point curve: (64, 0) and (192, 128).
    // Pixels below 64 should get scale 0 (no grain).
    // Pixels above 192 should get scale 128.
    // Pixel exactly at 128 should get scale 64 (midpoint).
    FilmGrainSynthesizer s;
    FilmGrainParams p = make_base_params();
    p.num_y_points = 2;
    p.scaling_points_y[0] = {64,  0};
    p.scaling_points_y[1] = {192, 128};

    // Fill with values that map to scale=0 so luma should be unchanged.
    Frame420 f_zero_scale(64, 64, /*fill=*/32);
    ASSERT_EQ(f_zero_scale.run(s, p), 0);
    EXPECT_EQ(f_zero_scale.dy, f_zero_scale.y)
        << "scale=0 region: luma must not change";

    // Fill with values that map to scale=128 so grain should be applied.
    Frame420 f_full_scale(64, 64, /*fill=*/200);
    FilmGrainSynthesizer s2;
    ASSERT_EQ(f_full_scale.run(s2, p), 0);
    EXPECT_NE(f_full_scale.dy, f_full_scale.y)
        << "scale=128 region: luma must change";
}

// --- fg_dbg helpers smoke test ---------------------------------------------

TEST(FilmGrainDebug, DumpParamsDoesNotCrash) {
    FilmGrainParams p = make_base_params();
    add_y_scaling(p);
    add_cb_scaling(p);
    add_cr_scaling(p);
    p.ar_coeff_lag = 1;
    p.ar_coeff_shift = 6;

    // Just verify it runs without crashing; output goes to /dev/null.
    FILE* devnull = fopen("/dev/null", "w");
    ASSERT_NE(devnull, nullptr);
    fg_dbg_dump_params(p, devnull);
    fclose(devnull);
}
