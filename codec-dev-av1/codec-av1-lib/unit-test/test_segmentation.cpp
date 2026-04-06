#include <gtest/gtest.h>
#include "../segmentation.hpp"
#include "../av1_seg_dbg.h"
#include "av1_seg_test_data.h"

using namespace av1::seg;
using namespace av1::seg::test;

// ============================================================================
// Basic operations
// ============================================================================

TEST(Seg, ClearAllFeatures) {
    Av1SegmentationData seg;
    seg.feature_mask[0] = 0xFF;
    seg.feature_data[0][0] = 5;
    seg.last_active_segid = 3;
    seg.clearAllFeatures();
    for (int i = 0; i < kMaxSegments; i++) {
        EXPECT_EQ(seg.feature_mask[i], 0u);
        for (int j = 0; j < kMax; j++)
            EXPECT_EQ(seg.feature_data[i][j], 0);
    }
    EXPECT_EQ(seg.last_active_segid, 0);
}

TEST(Seg, EnableAndCheckFeature) {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.enableFeature(2, kAltQ);
    EXPECT_TRUE(seg.isFeatureActive(2, kAltQ));
    EXPECT_FALSE(seg.isFeatureActive(2, kSkip));
    EXPECT_EQ(seg.feature_mask[2], (1u << kAltQ));
}

TEST(Seg, DisabledMeansInactive) {
    Av1SegmentationData seg{};
    seg.enabled = 0;
    seg.enableFeature(3, kRefFrame);
    EXPECT_FALSE(seg.isFeatureActive(3, kRefFrame));
}

TEST(Seg, SetAndGetData) {
    Av1SegmentationData seg{};
    seg.setData(4, kAltQ, -25);
    EXPECT_EQ(seg.getData(4, kAltQ), -25);
}

TEST(Seg, CalculateLastActive) {
    Av1SegmentationData seg{};
    seg.enableFeature(1, kAltQ);
    seg.enableFeature(5, kSkip);
    seg.calculateLastActive();
    EXPECT_EQ(seg.last_active_segid, 5);
}

TEST(Seg, CopyFeatures) {
    auto src = fullFeatures();
    Av1SegmentationData dst{};
    dst.copyFeaturesFrom(src);
    EXPECT_EQ(dst.feature_mask[1], src.feature_mask[1]);
    EXPECT_EQ(dst.feature_data[1][kAltQ], -20);
    EXPECT_EQ(dst.feature_data[7][kAltQ], 50);
    EXPECT_EQ(dst.last_active_segid, 7);
    EXPECT_EQ(dst.segid_preskip, 1);
    // enabled/update flags are NOT copied
    EXPECT_EQ(dst.enabled, 0);
}

// ============================================================================
// Bounds checking
// ============================================================================

TEST(Seg, OutOfBoundsSegmentId) {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.enableFeature(kMaxSegments + 1, kAltQ);
    EXPECT_FALSE(seg.isFeatureActive(kMaxSegments + 1, kAltQ));
    seg.setData(kMaxSegments, kAltQ, 10);
    EXPECT_EQ(seg.getData(kMaxSegments, kAltQ), 0);
}

TEST(Seg, OutOfBoundsFeatureId) {
    Av1SegmentationData seg{};
    seg.enabled = 1;
    seg.enableFeature(0, static_cast<SegLvlFeature>(kMax + 1));
    EXPECT_FALSE(seg.isFeatureActive(0, static_cast<SegLvlFeature>(kMax + 1)));
}

TEST(Seg, NegativeIndices) {
    Av1SegmentationData seg{};
    seg.enableFeature(-1, kAltQ);
    EXPECT_EQ(seg.getData(-1, kAltQ), 0);
    seg.setData(0, static_cast<SegLvlFeature>(-1), 42);
    EXPECT_EQ(seg.getData(0, static_cast<SegLvlFeature>(-1)), 0);
}

// ============================================================================
// AOM feature tables — §5.9.14
// ============================================================================

TEST(Seg, FeatureSignedTable) {
    EXPECT_TRUE(Av1SegmentationData::isFeatureSigned(kAltQ));
    EXPECT_TRUE(Av1SegmentationData::isFeatureSigned(kAltLfYV));
    EXPECT_TRUE(Av1SegmentationData::isFeatureSigned(kAltLfYH));
    EXPECT_TRUE(Av1SegmentationData::isFeatureSigned(kAltLfU));
    EXPECT_TRUE(Av1SegmentationData::isFeatureSigned(kAltLfV));
    EXPECT_FALSE(Av1SegmentationData::isFeatureSigned(kRefFrame));
    EXPECT_FALSE(Av1SegmentationData::isFeatureSigned(kSkip));
    EXPECT_FALSE(Av1SegmentationData::isFeatureSigned(kGlobalMv));
}

TEST(Seg, FeatureDataMaxTable) {
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kAltQ), 255);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kAltLfYV), 63);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kAltLfYH), 63);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kAltLfU), 63);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kAltLfV), 63);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kRefFrame), 7);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kSkip), 0);
    EXPECT_EQ(Av1SegmentationData::featureDataMax(kGlobalMv), 0);
}

// ============================================================================
// Clamping
// ============================================================================

TEST(Seg, ClampSignedFeature) {
    // kAltQ: signed, max=255
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltQ, 300), 255);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltQ, -300), -255);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltQ, 100), 100);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltQ, -100), -100);
    // kAltLfYV: signed, max=63
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltLfYV, 100), 63);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kAltLfYV, -100), -63);
}

TEST(Seg, ClampUnsignedFeature) {
    // kRefFrame: unsigned, max=7
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kRefFrame, 10), 7);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kRefFrame, -1), 0);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kRefFrame, 5), 5);
    // kSkip: unsigned, max=0
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kSkip, 1), 0);
    EXPECT_EQ(Av1SegmentationData::clampFeatureValue(kSkip, -1), 0);
}

// ============================================================================
// Test data vectors
// ============================================================================

TEST(SegTestData, SingleSegAltQ) {
    auto seg = singleSegAltQ();
    EXPECT_TRUE(seg.isFeatureActive(0, kAltQ));
    EXPECT_EQ(seg.getData(0, kAltQ), -15);
    EXPECT_EQ(seg.last_active_segid, 0);
    // Other segments inactive
    for (int i = 1; i < kMaxSegments; i++)
        EXPECT_EQ(seg.feature_mask[i], 0u);
}

TEST(SegTestData, MultiSegROI) {
    auto seg = multiSegROI();
    EXPECT_EQ(seg.getData(0, kAltQ), 0);
    EXPECT_EQ(seg.getData(1, kAltQ), -10);
    EXPECT_EQ(seg.getData(2, kAltQ), -25);
    EXPECT_TRUE(seg.isFeatureActive(3, kSkip));
    EXPECT_EQ(seg.last_active_segid, 3);
}

TEST(SegTestData, FullFeatures) {
    auto seg = fullFeatures();
    EXPECT_EQ(seg.temporal_update, 1);
    EXPECT_EQ(seg.segid_preskip, 1);
    // Segment 1: alt Q + alt LF
    EXPECT_TRUE(seg.isFeatureActive(1, kAltQ));
    EXPECT_TRUE(seg.isFeatureActive(1, kAltLfYV));
    EXPECT_TRUE(seg.isFeatureActive(1, kAltLfYH));
    EXPECT_EQ(seg.getData(1, kAltQ), -20);
    // Segment 2: ref frame
    EXPECT_TRUE(seg.isFeatureActive(2, kRefFrame));
    EXPECT_EQ(seg.getData(2, kRefFrame), 3);
    // Segment 5: global MV
    EXPECT_TRUE(seg.isFeatureActive(5, kGlobalMv));
    // Segment 7: all LF planes + Q
    EXPECT_TRUE(seg.isFeatureActive(7, kAltQ));
    EXPECT_TRUE(seg.isFeatureActive(7, kAltLfU));
    EXPECT_TRUE(seg.isFeatureActive(7, kAltLfV));
    EXPECT_EQ(seg.last_active_segid, 7);
}

TEST(SegTestData, AllSegmentsSkip) {
    auto seg = allSegmentsSkip();
    for (int i = 0; i < kMaxSegments; i++)
        EXPECT_TRUE(seg.isFeatureActive(i, kSkip));
    EXPECT_EQ(seg.last_active_segid, 7);
}

TEST(SegTestData, BoundaryValues) {
    auto seg = boundaryValues();
    EXPECT_EQ(seg.getData(0, kAltQ), 255);
    EXPECT_EQ(seg.getData(1, kAltQ), -255);
    EXPECT_EQ(seg.getData(2, kAltLfYV), 63);
    EXPECT_EQ(seg.getData(3, kAltLfYV), -63);
    EXPECT_EQ(seg.getData(4, kRefFrame), 7);
}

TEST(SegTestData, DisabledSegmentation) {
    auto seg = disabled();
    // Feature is set but seg is disabled → inactive
    EXPECT_FALSE(seg.isFeatureActive(0, kAltQ));
    // Data is still there if you query raw
    EXPECT_EQ(seg.getData(0, kAltQ), -30);
}

// ============================================================================
// Debug dump smoke test
// ============================================================================

TEST(SegDebug, DumpDoesNotCrash) {
    FILE* devnull = fopen("/dev/null", "w");
    ASSERT_NE(devnull, nullptr);
    segDbgDumpParams(fullFeatures(), devnull);
    segDbgDumpParams(disabled(), devnull);
    segDbgDumpParams(allSegmentsSkip(), devnull);
    fclose(devnull);
}
