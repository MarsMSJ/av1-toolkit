#include <gtest/gtest.h>
#include "../segmentation.hpp"

using namespace av1;

TEST(SegmentationTest, ClearAllSegFeatures) {
    SegmentationData seg;
    seg.feature_mask[0] = 0xFF;
    seg.feature_data[0][0] = 5;
    seg.last_active_segid = 3;

    clearall_segfeatures(seg);

    for (int i = 0; i < MAX_SEGMENTS; i++) {
        EXPECT_EQ(seg.feature_mask[i], 0);
        for (int j = 0; j < SEG_LVL_MAX; j++) {
            EXPECT_EQ(seg.feature_data[i][j], 0);
        }
    }
    EXPECT_EQ(seg.last_active_segid, 0);
}

TEST(SegmentationTest, EnableSegFeature) {
    SegmentationData seg;
    clearall_segfeatures(seg);
    
    enable_segfeature(seg, 2, SEG_LVL_ALT_Q);
    EXPECT_EQ(seg.feature_mask[2], (1u << SEG_LVL_ALT_Q));
}

TEST(SegmentationTest, SegFeatureActive) {
    SegmentationData seg;
    clearall_segfeatures(seg);
    seg.enabled = true;
    
    enable_segfeature(seg, 3, SEG_LVL_REF_FRAME);
    EXPECT_TRUE(segfeature_active(seg, 3, SEG_LVL_REF_FRAME));
    EXPECT_FALSE(segfeature_active(seg, 3, SEG_LVL_SKIP));
    
    seg.enabled = false;
    EXPECT_FALSE(segfeature_active(seg, 3, SEG_LVL_REF_FRAME));
}

TEST(SegmentationTest, SetAndGetSegData) {
    SegmentationData seg;
    clearall_segfeatures(seg);
    
    set_segdata(seg, 4, SEG_LVL_ALT_Q, -25);
    EXPECT_EQ(get_segdata(seg, 4, SEG_LVL_ALT_Q), -25);
}

TEST(SegmentationTest, CalculateSegData) {
    SegmentationData seg;
    clearall_segfeatures(seg);
    
    enable_segfeature(seg, 1, SEG_LVL_ALT_Q);
    enable_segfeature(seg, 5, SEG_LVL_SKIP);
    
    calculate_segdata(seg);
    EXPECT_EQ(seg.last_active_segid, 5);
}

TEST(SegmentationTest, SegFeaturesCopy) {
    SegmentationData src, dst;
    clearall_segfeatures(src);
    clearall_segfeatures(dst);
    
    src.enabled = true;
    enable_segfeature(src, 2, SEG_LVL_GLOBALMV);
    set_segdata(src, 2, SEG_LVL_GLOBALMV, 1);
    calculate_segdata(src);
    src.segid_preskip = true;
    
    segfeatures_copy(dst, src);
    
    EXPECT_EQ(dst.feature_mask[2], (1u << SEG_LVL_GLOBALMV));
    EXPECT_EQ(dst.feature_data[2][SEG_LVL_GLOBALMV], 1);
    EXPECT_TRUE(dst.segid_preskip);
    EXPECT_EQ(dst.last_active_segid, 2);
}

TEST(SegmentationTest, ErrorChecks) {
    SegmentationData seg;
    clearall_segfeatures(seg);
    
    // Out of bounds segment id
    enable_segfeature(seg, MAX_SEGMENTS + 1, SEG_LVL_ALT_Q);
    EXPECT_FALSE(segfeature_active(seg, MAX_SEGMENTS + 1, SEG_LVL_ALT_Q));
    
    set_segdata(seg, MAX_SEGMENTS, SEG_LVL_ALT_Q, 10);
    EXPECT_EQ(get_segdata(seg, MAX_SEGMENTS, SEG_LVL_ALT_Q), 0);
    
    // Out of bounds feature id
    enable_segfeature(seg, 0, static_cast<SegLvlFeatures>(SEG_LVL_MAX + 1));
    EXPECT_FALSE(segfeature_active(seg, 0, static_cast<SegLvlFeatures>(SEG_LVL_MAX + 1)));
}
