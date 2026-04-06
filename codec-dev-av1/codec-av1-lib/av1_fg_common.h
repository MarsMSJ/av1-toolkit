
 #pragma once
 #include <array>
 #include <cstddef>
 #include <cstdint>
 
 namespace av1::fg {
 
 template <typename T>
 struct XY
 {
     T x;
     T y;
 };

 using IntXY = XY<int>;
 using SizeXY = XY<std::size_t>;
 using Uint8XY = XY<std::uint8_t>;
 using Uint16XY = XY<std::uint16_t>;

 // [MMSJ] §7.18.3.4
 using PredictionPositionTable = std::array<std::array<int, 3>, 25>;
using FilmGrainTemplate = std::array<int, 256>;

 struct FilmGrainFrameTemplate
 {
     FilmGrainTemplate scaling_lut_y{};
     FilmGrainTemplate scaling_lut_cb{};
     FilmGrainTemplate scaling_lut_cr{};
 };
 
 // [MMSJ] §5.9.30 / AOM: aom_dsp/grain_params.h

 struct Av1FilmGrainSynthesisData
 {
     // clang-format off
     int                                    apply_grain;               // u(1)
     int                                    update_parameters;         // AOM extension (not in AV1 spec)
 
     // Luma grain
     int                                    num_y_points;              // u(4), [0..14]
     std::array<std::array<int32_t, 2>, 14> scaling_points_y;          // u(8) each: (value, scaling)
 
     // Chroma grain CB
     int                                    chroma_scaling_from_luma;  // u(1)
     int                                    num_cb_points;             // u(4), [0..10] — zero if chroma_scaling_from_luma
     std::array<std::array<int, 2>, 10>     scaling_points_cb;         // u(8) each
 
     // Chroma grain CR
     int                                    num_cr_points;             // u(4), [0..10] — zero if chroma_scaling_from_luma
     std::array<std::array<int, 2>, 10>     scaling_points_cr;         // u(8) each
 
     // AR coefficients
     int                                    ar_coeff_lag;           // u(2), [0..3]
     std::array<int, 24>                    ar_coeffs_y;            // i(8), used: 0..(2*lag*(lag+1))
     std::array<int, 25>                    ar_coeffs_cb;
     std::array<int, 25>                    ar_coeffs_cr;
     int                                    ar_coeff_shift;         // u(2), actual shift = 6 + value ∈ [6..9]
     int                                    cb_mult, cb_luma_mult;  // u(8) each
     std::uint16_t                          cb_offset : 9;          // u(9)
     int                                    cr_mult, cr_luma_mult;  // u(8) each
     std::uint16_t                          cr_offset : 9;          // u(9)
     int                                    scaling_shift;          // u(2), actual shift = 7 + value ∈ [7..10]
 
     // Other parameters
     int                                    overlap_flag;
     int                                    clip_to_restricted_range;
     int                                    bit_depth;              // video bit depth (8/10/12)
     int                                    grain_scale_shift;      // u(2)
     std::uint16_t                          random_seed;            // u(16)
     // clang-format on
 
     constexpr unsigned ar_coeff_shift_value() const noexcept
     {
         return 6u + static_cast<unsigned>(ar_coeff_shift);
     }
 
     constexpr unsigned scaling_shift_value() const noexcept
     {
         return 7u + static_cast<unsigned>(scaling_shift);
     }
 
     std::size_t num_ar_coeffs_y() const noexcept
     {
         return 2 * ar_coeff_lag * (ar_coeff_lag + 1) + 1;
     }
 
     std::size_t num_ar_coeffs_cb_cr() const noexcept
     {
         return 2 * ar_coeff_lag * (ar_coeff_lag + 1) + 2;
     }
 };
 
 }  // namespace av1::fg
 