#include "av1_fg_dbg.h"

namespace av1::fg {

void fgDbgDumpParams(const Av1FilmGrainSynthesisData& fg, FILE* out) {
    fprintf(out, "[FILM_GRAIN]\n");
    fprintf(out, "  apply_grain           : %d\n", fg.apply_grain);
    if (!fg.apply_grain)
        return;

    fprintf(out, "  update_parameters     : %d\n", fg.update_parameters);
    fprintf(out, "  random_seed           : %u\n",
            static_cast<unsigned int>(fg.random_seed));
    fprintf(out, "  bit_depth             : %d\n", fg.bit_depth);
    fprintf(out, "  scaling_shift         : %d\n", fg.scaling_shift);
    fprintf(out, "  ar_coeff_lag          : %d\n", fg.ar_coeff_lag);
    fprintf(out, "  ar_coeff_shift        : %d\n", fg.ar_coeff_shift);
    fprintf(out, "  grain_scale_shift     : %d\n", fg.grain_scale_shift);
    fprintf(out, "  overlap_flag          : %d\n", fg.overlap_flag);
    fprintf(out, "  clip_to_restricted    : %d\n", fg.clip_to_restricted_range);
    fprintf(out, "  chroma_from_luma      : %d\n", fg.chroma_scaling_from_luma);
    fprintf(out, "  cb_mult/luma_mult/off : %d / %d / %d\n",
            fg.cb_mult, fg.cb_luma_mult, fg.cb_offset);
    fprintf(out, "  cr_mult/luma_mult/off : %d / %d / %d\n",
            fg.cr_mult, fg.cr_luma_mult, fg.cr_offset);

    fprintf(out, "  num_y_points          : %d\n", fg.num_y_points);
    for (int i = 0; i < fg.num_y_points; i++)
        fprintf(out, "    y_point[%d] = (%d, %d)\n", i,
                fg.scaling_points_y[i][0], fg.scaling_points_y[i][1]);

    fprintf(out, "  num_cb_points         : %d\n", fg.num_cb_points);
    for (int i = 0; i < fg.num_cb_points; i++)
        fprintf(out, "    cb_point[%d] = (%d, %d)\n", i,
                fg.scaling_points_cb[i][0], fg.scaling_points_cb[i][1]);

    fprintf(out, "  num_cr_points         : %d\n", fg.num_cr_points);
    for (int i = 0; i < fg.num_cr_points; i++)
        fprintf(out, "    cr_point[%d] = (%d, %d)\n", i,
                fg.scaling_points_cr[i][0], fg.scaling_points_cr[i][1]);

    const int num_pos = 2 * fg.ar_coeff_lag * (fg.ar_coeff_lag + 1);
    if (num_pos > 0) {
        fprintf(out, "  ar_coeffs_y[0..%d]    :", num_pos - 1);
        for (int i = 0; i < num_pos; i++)
            fprintf(out, " %d", fg.ar_coeffs_y[i]);
        fprintf(out, "\n");

        fprintf(out, "  ar_coeffs_cb[0..%d]   :", num_pos);
        for (int i = 0; i <= num_pos; i++)
            fprintf(out, " %d", fg.ar_coeffs_cb[i]);
        fprintf(out, "\n");

        fprintf(out, "  ar_coeffs_cr[0..%d]   :", num_pos);
        for (int i = 0; i <= num_pos; i++)
            fprintf(out, " %d", fg.ar_coeffs_cr[i]);
        fprintf(out, "\n");
    }
}

void fgDbgDumpGrainBlock(const char* label, const int* block,
                         int grain_stride, int offset_y, int offset_x,
                         int rows, int cols, FILE* out) {
    fprintf(out, "[GRAIN_BLOCK %s  offset=(%d,%d)  size=%dx%d]\n",
            label, offset_y, offset_x, rows, cols);
    for (int i = 0; i < rows; i++) {
        const int* row_ptr = block + (offset_y + i) * grain_stride + offset_x;
        for (int j = 0; j < cols; j++) {
            fprintf(out, "%5d", row_ptr[j]);
            if ((j & 15) == 15 || j == cols - 1)
                fprintf(out, "\n");
        }
    }
}

}  // namespace av1::fg