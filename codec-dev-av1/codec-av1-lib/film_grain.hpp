#pragma once

// Structures and functions sourced from AOM reference decoder: 
// - aom/aom_dsp/grain_params.h
// - aom/av1/decoder/grain_synthesis.h

#include <cstdint>
#include <array>
#include <cstring>
#include <vector>

namespace av1 {

// Equivalent to aom_film_grain_t in AOM reference code
// Specified in Section 6.8.20 (Film grain params syntax) of AV1 specification
struct FilmGrainParams {
    bool apply_grain = false;
    bool update_parameters = false;

    std::array<std::array<int, 2>, 14> scaling_points_y{};
    int num_y_points = 0; // 0..14

    std::array<std::array<int, 2>, 10> scaling_points_cb{};
    int num_cb_points = 0; // 0..10

    std::array<std::array<int, 2>, 10> scaling_points_cr{};
    int num_cr_points = 0; // 0..10

    int scaling_shift = 0; // 8..11
    int ar_coeff_lag = 0;  // 0..3

    std::array<int, 24> ar_coeffs_y{};
    std::array<int, 25> ar_coeffs_cb{};
    std::array<int, 25> ar_coeffs_cr{};

    int ar_coeff_shift = 0; // 6..9

    int cb_mult = 0;
    int cb_luma_mult = 0;
    int cb_offset = 0;

    int cr_mult = 0;
    int cr_luma_mult = 0;
    int cr_offset = 0;

    bool overlap_flag = false;
    bool clip_to_restricted_range = false;
    unsigned int bit_depth = 8;
    bool chroma_scaling_from_luma = false;
    int grain_scale_shift = 0;
    uint16_t random_seed = 0;
};

bool check_grain_params_equiv(const FilmGrainParams& pa, const FilmGrainParams& pb);

class FilmGrainSynthesizer {
public:
    FilmGrainSynthesizer() = default;

    int add_film_grain(const FilmGrainParams& params, 
                       const std::vector<uint8_t>& src_y, int src_y_stride,
                       const std::vector<uint8_t>& src_cb, int src_cb_stride,
                       const std::vector<uint8_t>& src_cr, int src_cr_stride,
                       std::vector<uint8_t>& dst_y, int dst_y_stride,
                       std::vector<uint8_t>& dst_cb, int dst_cb_stride,
                       std::vector<uint8_t>& dst_cr, int dst_cr_stride,
                       int width, int height,
                       int chroma_subsamp_x, int chroma_subsamp_y);

private:
    uint16_t random_register_ = 0;
    
    // Gaussian sequence with zero mean and std=512
    static const int gaussian_sequence[2048];
    static const int gauss_bits = 11;

    static const int min_luma_legal_range = 16;
    static const int max_luma_legal_range = 235;

    static const int min_chroma_legal_range = 16;
    static const int max_chroma_legal_range = 240;
    
    int get_random_number(int bits);
    void init_random_generator(int luma_line, uint16_t seed);
    
    static void init_scaling_function(const std::array<std::array<int, 2>, 14>& scaling_points,
                                      int num_points, std::array<int, 256>& scaling_lut);
    
    static int scale_LUT(const std::array<int, 256>& scaling_lut, int index, int bit_depth);

    void add_noise_to_block(const FilmGrainParams& params,
                            const std::vector<uint8_t>& luma_src, const std::vector<uint8_t>& cb_src, const std::vector<uint8_t>& cr_src,
                            int luma_stride, int chroma_stride,
                            const std::vector<int>& luma_grain, const std::vector<int>& cb_grain, const std::vector<int>& cr_grain,
                            int luma_grain_stride, int chroma_grain_stride,
                            int half_luma_height, int half_luma_width,
                            int bit_depth, int chroma_subsamp_y, int chroma_subsamp_x,
                            std::vector<uint8_t>& luma_dst, std::vector<uint8_t>& cb_dst, std::vector<uint8_t>& cr_dst);
    
    // Core synthesis structures
    std::array<int, 256> scaling_lut_y_{};
    std::array<int, 256> scaling_lut_cb_{};
    std::array<int, 256> scaling_lut_cr_{};
    int grain_min_ = 0;
    int grain_max_ = 0;
    int luma_subblock_size_y_ = 32;
    int luma_subblock_size_x_ = 32;
};

} // namespace av1
