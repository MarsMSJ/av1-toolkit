#include <gtest/gtest.h>
#include <vector>
#include "../film_grain.hpp"

using namespace av1;

TEST(FilmGrainTest, CheckGrainParamsEquiv) {
    FilmGrainParams pa;
    FilmGrainParams pb;
    
    // Default should be equivalent
    EXPECT_TRUE(check_grain_params_equiv(pa, pb));
    
    // Change a parameter
    pa.apply_grain = true;
    EXPECT_FALSE(check_grain_params_equiv(pa, pb));
    
    pb.apply_grain = true;
    EXPECT_TRUE(check_grain_params_equiv(pa, pb));
    
    // update_parameters is ignored by the check
    pa.update_parameters = true;
    EXPECT_TRUE(check_grain_params_equiv(pa, pb));
}

TEST(FilmGrainTest, AddFilmGrainCopySuccess) {
    FilmGrainSynthesizer synthesizer;
    FilmGrainParams params;
    params.apply_grain = false;
    
    int width = 16;
    int height = 16;
    int stride = 16;
    
    std::vector<uint8_t> src_y(width * height, 128);
    std::vector<uint8_t> src_cb(width * height / 2, 128);
    std::vector<uint8_t> src_cr(width * height / 2, 128);
    
    std::vector<uint8_t> dst_y(width * height, 0);
    std::vector<uint8_t> dst_cb(width * height / 2, 0);
    std::vector<uint8_t> dst_cr(width * height / 2, 0);
    
    int result = synthesizer.add_film_grain(params, 
                               src_y, stride, src_cb, stride, src_cr, stride,
                               dst_y, stride, dst_cb, stride, dst_cr, stride,
                               width, height, 1, 1);
                               
    EXPECT_EQ(result, 0);
    
    // Since apply_grain is false, it should just be a copy
    for (size_t i = 0; i < src_y.size(); ++i) {
        EXPECT_EQ(dst_y[i], src_y[i]);
    }
}

TEST(FilmGrainTest, AddFilmGrainErrors) {
    FilmGrainSynthesizer synthesizer;
    FilmGrainParams params;
    
    int width = 16;
    int height = 16;
    int stride = 16;
    
    std::vector<uint8_t> src_y(width * height, 128);
    std::vector<uint8_t> src_cb(width * height / 2, 128);
    std::vector<uint8_t> src_cr(width * height / 2, 128);
    
    std::vector<uint8_t> dst_y(width * height, 0);
    std::vector<uint8_t> dst_cb(width * height / 2, 0);
    std::vector<uint8_t> dst_cr(width * height / 2, 0);
    
    // Test empty vectors
    std::vector<uint8_t> empty_vector;
    EXPECT_EQ(synthesizer.add_film_grain(params, empty_vector, stride, src_cb, stride, src_cr, stride, dst_y, stride, dst_cb, stride, dst_cr, stride, width, height, 1, 1), -1);
    
    // Test insufficient size
    std::vector<uint8_t> small_vector(10, 0);
    EXPECT_EQ(synthesizer.add_film_grain(params, src_y, stride, small_vector, stride, src_cr, stride, dst_y, stride, dst_cb, stride, dst_cr, stride, width, height, 1, 1), -1);
}
