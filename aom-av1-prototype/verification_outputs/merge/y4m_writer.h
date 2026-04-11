#ifndef Y4M_WRITER_H
#define Y4M_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Y4MWriter Y4MWriter;

Y4MWriter *y4m_writer_open(const char *filename, int width, int height,
                            int fps_n, int fps_d, int bit_depth, int chroma_subsampling);
int y4m_writer_close(Y4MWriter *writer);
int y4m_writer_write_frame(Y4MWriter *writer, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                            int y_stride, int u_stride, int v_stride);
int y4m_writer_write_buffer(Y4MWriter *writer, const struct Av1OutputBuffer *buffer);
uint64_t y4m_writer_get_frame_count(const Y4MWriter *writer);
bool y4m_writer_is_valid(const Y4MWriter *writer);

#ifdef __cplusplus
}
#endif

#endif /* Y4M_WRITER_H */
