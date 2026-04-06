#include <gtest/gtest.h>
#include <cmath>
#include <numeric>
#include "../film_grain.hpp"
#include "av1_fg_test_data.h"

using namespace av1;
using namespace av1::fg;
using namespace av1::fg::test;

// ============================================================================
// PRNG (fg_prng.h) — §7.18.3.1
// ============================================================================

TEST(FgPrng, DeterministicOutput) {
    Av1FgPrng a(12345), b(12345);
    for (int i = 0; i < 100; i++)
        EXPECT_EQ(a.get(11), b.get(11));
}

TEST(FgPrng, DifferentSeedsDiffer) {
    Av1FgPrng a(100), b(101);
    bool differ = false;
    for (int i = 0; i < 50; i++)
        if (a.get(11) != b.get(11)) differ = true;
    EXPECT_TRUE(differ);
}

TEST(FgPrng, CbCrStripesDiffer) {
    Av1FgPrng a, b;
    a.init(Av1FgPrng::kCbStripeIdx, 7391);
    b.init(Av1FgPrng::kCrStripeIdx, 7391);
    bool differ = false;
    for (int i = 0; i < 50; i++)
        if (a.get(11) != b.get(11)) differ = true;
    EXPECT_TRUE(differ);
}

TEST(FgPrng, OutputRange) {
    Av1FgPrng prng(42);
    for (int i = 0; i < 500; i++) {
        int val = prng.get(11);
        EXPECT_GE(val, 0);
        EXPECT_LT(val, 2048);
    }
}

// ============================================================================
// Gaussian sequence (av1_fg_gaussian.h) — §7.18.3.2
// ============================================================================

TEST(FgGaussian, TableSize) {
    EXPECT_EQ(sizeof(kGaussianSequence) / sizeof(kGaussianSequence[0]), 2048u);
}

TEST(FgGaussian, ValueRange12Bit) {
    for (int i = 0; i < 2048; i++) {
        EXPECT_GE(kGaussianSequence[i], -2048);
        EXPECT_LE(kGaussianSequence[i], 2047);
    }
}

TEST(FgGaussian, ApproximatelyZeroMean) {
    long long sum = 0;
    for (int i = 0; i < 2048; i++) sum += kGaussianSequence[i];
    double mean = static_cast<double>(sum) / 2048.0;
    EXPECT_NEAR(mean, 0.0, 15.0);
}

// ============================================================================
// Scaling LUT (av1_fg_scaling.h) — §7.18.3.2, §7.18.3.3
// ============================================================================

TEST(FgScaling, FlatCurve) {
    std::array<int, 2> pts[2] = {{0, 100}, {255, 100}};
    std::array<int, 256> lut{};
    initScalingFunction(pts, 2, lut);
    for (int i = 0; i < 256; i++)
        EXPECT_EQ(lut[i], 100);
}

TEST(FgScaling, LinearRamp) {
    std::array<int, 2> pts[2] = {{0, 0}, {255, 255}};
    std::array<int, 256> lut{};
    initScalingFunction(pts, 2, lut);
    for (int i = 0; i < 256; i++)
        EXPECT_NEAR(lut[i], i, 1);
}

TEST(FgScaling, AomVector1LumaLUT) {
    // [MMSJ] Verify scaling LUT from AOM test vector 1
    auto p = aomVector1();
    std::array<int, 256> lut{};
    synthesizeScalingLutY(p, lut);
    // Before first point (16): should be 0
    EXPECT_EQ(lut[0], 0);
    EXPECT_EQ(lut[15], 0);
    // At first point (16,0): should be 0
    EXPECT_EQ(lut[16], 0);
    // After last point (178): should be 184
    EXPECT_EQ(lut[200], 184);
    EXPECT_EQ(lut[255], 184);
    // Monotonicity not guaranteed (curve can dip), but values should be in [0,255]
    for (int i = 0; i < 256; i++) {
        EXPECT_GE(lut[i], 0);
        EXPECT_LE(lut[i], 255);
    }
}

TEST(FgScaling, AomVector6LumaRampDown) {
    // [MMSJ] Vector 6: ramp from 96 down to 0
    auto p = aomVector6LumaOnly();
    std::array<int, 256> lut{};
    synthesizeScalingLutY(p, lut);
    EXPECT_EQ(lut[0], 96);
    EXPECT_EQ(lut[255], 0);
    // Should generally decrease
    EXPECT_GT(lut[0], lut[255]);
}

TEST(FgScaling, ChromaFromLuma) {
    auto p = chromaFromLumaVector();
    std::array<int, 256> lut_y{}, lut_cb{}, lut_cr{};
    synthesizeScalingLutY(p, lut_y);
    synthesizeScalingLutCb(p, lut_cb, lut_y);
    synthesizeScalingLutCr(p, lut_cr, lut_y);
    EXPECT_EQ(lut_cb, lut_y);
    EXPECT_EQ(lut_cr, lut_y);
}

TEST(FgScaling, ScaleLUT8bit) {
    std::array<int, 256> lut{};
    for (int i = 0; i < 256; i++) lut[i] = i * 2;
    EXPECT_EQ(scaleLUT(lut, 100, 8), 200);
    EXPECT_EQ(scaleLUT(lut, 0, 8), 0);
    EXPECT_EQ(scaleLUT(lut, 255, 8), 510);
}

// ============================================================================
// Constants (av1_fg_aom_const.h) — AOM: av1/decoder/grain_synthesis.c
// ============================================================================

TEST(FgConstants, LumaBlockSize) {
    auto [bx, by] = kAomConstants::getLumaBlockSize();
    EXPECT_EQ(bx, 82);
    EXPECT_EQ(by, 73);
}

TEST(FgConstants, ChromaBlockSize420) {
    auto [bx, by] = kAomConstants::getChromaBlockSize({1, 1});
    EXPECT_EQ(bx, 44);
    EXPECT_EQ(by, 38);
}

TEST(FgConstants, ChromaBlockSize444) {
    auto [bx, by] = kAomConstants::getChromaBlockSize({0, 0});
    EXPECT_EQ(bx, 82);
    EXPECT_EQ(by, 73);
}

TEST(FgConstants, PredTableLag2) {
    // [MMSJ] lag=2 → num_pos = 2*2*3 = 12
    PredictionPositionTable luma{}, chroma{};
    kAomConstants::buildPositionPredictionTables(2, 5, luma, chroma);
    // First entry: (-2,-2,0)
    EXPECT_EQ(luma[0][0], -2); EXPECT_EQ(luma[0][1], -2);
    // Chroma coupling at position 12
    EXPECT_EQ(chroma[12][2], 1);
}

TEST(FgConstants, PredTableLag3) {
    // [MMSJ] lag=3 → num_pos = 2*3*4 = 24
    PredictionPositionTable luma{}, chroma{};
    kAomConstants::buildPositionPredictionTables(3, 2, luma, chroma);
    EXPECT_EQ(luma[0][0], -3); EXPECT_EQ(luma[0][1], -3);
    EXPECT_EQ(chroma[24][2], 1); // luma coupling
}

// ============================================================================
// Grain generation (av1_fg_grain_gen.h) — §7.18.3.2
// ============================================================================

TEST(FgGrainGen, LumaZeroPointsZerosBlock) {
    auto p = aomVector6LumaOnly();
    p.num_y_points = 0;  // override to zero
    PredictionPositionTable pred{};
    Av1FgPrng prng(p.random_seed);
    auto [bx, by] = kAomConstants::getLumaBlockSize();
    std::array<int, kAomConstants::kLumaGrainSamples> block{};
    block.fill(99);
    generateLumaGrainBlock(p, pred, 0, prng, block.data(), by, bx, bx, -128, 127);
    for (int i = 0; i < by * bx; i++)
        EXPECT_EQ(block[i], 0);
}

TEST(FgGrainGen, AomVector1LumaGrainInRange) {
    auto p = aomVector1();
    int num_pos = 2 * p.ar_coeff_lag * (p.ar_coeff_lag + 1);
    PredictionPositionTable luma_pred{}, chroma_pred{};
    kAomConstants::buildPositionPredictionTables(p.ar_coeff_lag, p.num_y_points, luma_pred, chroma_pred);

    Av1FgPrng prng(p.random_seed);
    auto [bx, by] = kAomConstants::getLumaBlockSize();
    std::array<int, kAomConstants::kLumaGrainSamples> block{};
    generateLumaGrainBlock(p, luma_pred, num_pos, prng, block.data(), by, bx, bx, -128, 127);

    for (int i = 0; i < by * bx; i++) {
        EXPECT_GE(block[i], -128);
        EXPECT_LE(block[i], 127);
    }
}

TEST(FgGrainGen, AomVector2LumaGrainDeterministic) {
    auto p = aomVector2();
    int num_pos = 2 * p.ar_coeff_lag * (p.ar_coeff_lag + 1);
    PredictionPositionTable luma_pred{}, dummy{};
    kAomConstants::buildPositionPredictionTables(p.ar_coeff_lag, p.num_y_points, luma_pred, dummy);

    auto [bx, by] = kAomConstants::getLumaBlockSize();
    std::array<int, kAomConstants::kLumaGrainSamples> b1{}, b2{};
    Av1FgPrng prng1(p.random_seed), prng2(p.random_seed);
    generateLumaGrainBlock(p, luma_pred, num_pos, prng1, b1.data(), by, bx, bx, -128, 127);
    generateLumaGrainBlock(p, luma_pred, num_pos, prng2, b2.data(), by, bx, bx, -128, 127);
    EXPECT_EQ(b1, b2);
}

TEST(FgGrainGen, AomVector1ChromaGrainInRange) {
    auto p = aomVector1();
    IntXY sub = {1, 1};
    auto [lbx, lby] = kAomConstants::getLumaBlockSize();
    auto [cbx, cby] = kAomConstants::getChromaBlockSize(sub);
    int num_pos_l = 2 * p.ar_coeff_lag * (p.ar_coeff_lag + 1);
    int num_pos_c = num_pos_l + (p.num_y_points > 0 ? 1 : 0);

    PredictionPositionTable luma_pred{}, chroma_pred{};
    kAomConstants::buildPositionPredictionTables(p.ar_coeff_lag, p.num_y_points, luma_pred, chroma_pred);

    Av1FgPrng prng(p.random_seed);
    std::array<int, kAomConstants::kLumaGrainSamples> luma_block{};
    generateLumaGrainBlock(p, luma_pred, num_pos_l, prng, luma_block.data(), lby, lbx, lbx, -128, 127);

    std::array<int, kAomConstants::kMaxChromaGrainSamples> cb{}, cr{};
    generateChromaGrainBlocks(p, chroma_pred, num_pos_c, prng, luma_block.data(),
                              cb.data(), cr.data(), lbx, cby, cbx, cbx, 1, 1, -128, 127);

    for (int i = 0; i < cby * cbx; i++) {
        EXPECT_GE(cb[i], -128); EXPECT_LE(cb[i], 127);
        EXPECT_GE(cr[i], -128); EXPECT_LE(cr[i], 127);
    }
}

TEST(FgGrainGen, ARFilterChangesGrain) {
    // [MMSJ] Same seed, with vs without AR filter
    auto p_no = aomVector1();
    p_no.ar_coeff_lag = 0;
    auto p_ar = aomVector1();

    auto [bx, by] = kAomConstants::getLumaBlockSize();
    std::array<int, kAomConstants::kLumaGrainSamples> b_no{}, b_ar{};

    PredictionPositionTable pred_no{}, pred_ar{}, dummy{};
    kAomConstants::buildPositionPredictionTables(0, p_no.num_y_points, pred_no, dummy);
    int num_pos_ar = 2 * p_ar.ar_coeff_lag * (p_ar.ar_coeff_lag + 1);
    kAomConstants::buildPositionPredictionTables(p_ar.ar_coeff_lag, p_ar.num_y_points, pred_ar, dummy);

    Av1FgPrng prng1(p_no.random_seed), prng2(p_ar.random_seed);
    generateLumaGrainBlock(p_no, pred_no, 0, prng1, b_no.data(), by, bx, bx, -128, 127);
    generateLumaGrainBlock(p_ar, pred_ar, num_pos_ar, prng2, b_ar.data(), by, bx, bx, -128, 127);
    EXPECT_NE(b_no, b_ar);
}

// ============================================================================
// Full synthesizer (film_grain.hpp)
// ============================================================================

TEST(FgSynthesizer, AomVector1Synthesize) {
    FilmGrainSynthesizer synth;
    auto p = aomVector1();
    EXPECT_EQ(synth.synthesize(p, {1, 1}), 0);
    const auto& out = synth.output();
    EXPECT_EQ(out.luma_block_size_x, 82);
    EXPECT_EQ(out.luma_block_size_y, 73);
    EXPECT_EQ(out.chroma_block_size_x, 44);
    EXPECT_EQ(out.chroma_block_size_y, 38);
}

TEST(FgSynthesizer, AomVector2Synthesize) {
    FilmGrainSynthesizer synth;
    EXPECT_EQ(synth.synthesize(aomVector2(), {1, 1}), 0);
    // Grain templates must be non-zero
    bool any = false;
    for (int v : synth.output().luma_grain) if (v != 0) { any = true; break; }
    EXPECT_TRUE(any);
}

TEST(FgSynthesizer, AomVector3GrainScaleShift) {
    // [MMSJ] grain_scale_shift=1 reduces grain amplitude
    FilmGrainSynthesizer s_shift, s_no_shift;
    auto p_shift = aomVector3();
    auto p_no_shift = aomVector3();
    p_no_shift.grain_scale_shift = 0;
    s_shift.synthesize(p_shift, {1, 1});
    s_no_shift.synthesize(p_no_shift, {1, 1});
    EXPECT_NE(s_shift.output().luma_grain, s_no_shift.output().luma_grain);
}

TEST(FgSynthesizer, AomVector6LumaOnlyNoChromaGrain) {
    // [MMSJ] Zero chroma points → chroma grain should be all zeros
    FilmGrainSynthesizer synth;
    synth.synthesize(aomVector6LumaOnly(), {1, 1});
    const auto& out = synth.output();
    bool cb_nonzero = false, cr_nonzero = false;
    for (int i = 0; i < out.chroma_block_size_y * out.chroma_block_size_x; i++) {
        if (out.cb_grain[i] != 0) cb_nonzero = true;
        if (out.cr_grain[i] != 0) cr_nonzero = true;
    }
    EXPECT_FALSE(cb_nonzero);
    EXPECT_FALSE(cr_nonzero);
    // But luma should have grain
    bool luma_nonzero = false;
    for (int v : out.luma_grain) if (v != 0) { luma_nonzero = true; break; }
    EXPECT_TRUE(luma_nonzero);
}

TEST(FgSynthesizer, ChromaFromLumaLUTs) {
    FilmGrainSynthesizer synth;
    synth.synthesize(chromaFromLumaVector(), {1, 1});
    const auto& out = synth.output();
    EXPECT_EQ(out.scaling_lut_cb, out.scaling_lut_y);
    EXPECT_EQ(out.scaling_lut_cr, out.scaling_lut_y);
}

TEST(FgSynthesizer, Deterministic) {
    auto p = aomVector1();
    FilmGrainSynthesizer s1, s2;
    s1.synthesize(p, {1, 1});
    s2.synthesize(p, {1, 1});
    EXPECT_EQ(s1.output().luma_grain, s2.output().luma_grain);
    EXPECT_EQ(s1.output().cb_grain, s2.output().cb_grain);
    EXPECT_EQ(s1.output().scaling_lut_y, s2.output().scaling_lut_y);
}

TEST(FgSynthesizer, DifferentSeedsDiffer) {
    auto p1 = aomVector1();
    auto p2 = aomVector1();
    p2.random_seed = 1063;
    FilmGrainSynthesizer s1, s2;
    s1.synthesize(p1, {1, 1});
    s2.synthesize(p2, {1, 1});
    EXPECT_NE(s1.output().luma_grain, s2.output().luma_grain);
}

TEST(FgSynthesizer, ApplyGrainFalse) {
    Av1FilmGrainSynthesisData p{};
    p.apply_grain = 0;
    FilmGrainSynthesizer synth;
    EXPECT_EQ(synth.synthesize(p, {1, 1}), 0);
}

// ============================================================================
// Debug dump smoke tests
// ============================================================================

TEST(FgDebug, DumpParamsDoesNotCrash) {
    auto p = aomVector1();
    FILE* devnull = fopen("/dev/null", "w");
    ASSERT_NE(devnull, nullptr);
    fgDbgDumpParams(p, devnull);
    fclose(devnull);
}

TEST(FgDebug, DumpGrainBlockDoesNotCrash) {
    FilmGrainSynthesizer synth;
    synth.synthesize(aomVector1(), {1, 1});
    FILE* devnull = fopen("/dev/null", "w");
    ASSERT_NE(devnull, nullptr);
    fgDbgDumpGrainBlock("luma", synth.output().luma_grain.data(), 82, 3, 3, 8, 8, devnull);
    fclose(devnull);
}
