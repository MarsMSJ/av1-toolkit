#ifndef Y4M_WRITER_H
#define Y4M_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Y4M Writer Handle
// ============================================================================

typedef struct Y4MWriter Y4MWriter;

// ============================================================================
// Y4M Colorspace Types
// ============================================================================

#define Y4M_COLORSPACE_YUV420 "420"
#define Y4M_COLORSPACE_YUV422 "422"
#define Y4M_COLORSPACE_YUV444 "444"
#define Y4M_COLORSPACE_YUV420P10 "420p10"
#define Y4M_COLORSPACE_YUV420P12 "420p12"

// ============================================================================
// API Functions
// ============================================================================

/**
 * Open a Y4M file for writing.
 * 
 * @param filename Path to output Y4M file
 * @param width    Frame width in pixels
 * @param height   Frame height in pixels
 * @param fps_n    Frame rate numerator (e.g., 30)
 * @param fps_d    Frame rate denominator (e.g., 1)
 * @param bit_depth Bit depth (8, 10, 12)
 * @param chroma_subsampling Chroma subsampling (0=420, 1=422, 2=444)
 * @return Writer handle on success, NULL on failure
 */
Y4MWriter *y4m_writer_open(const char *filename, 
                            int width, 
                            int height,
                            int fps_n, 
                            int fps_d,
                            int bit_depth,
                            int chroma_subsampling);

/**
 * Close a Y4M file and free resources.
 * 
 * @param writer Writer handle
 * @return 0 on success, -1 on error
 */
int y4m_writer_close(Y4MWriter *writer);

/**
 * Write a frame to the Y4M file.
 * 
 * @param writer Writer handle
 * @param y      Y plane data
 * @param u      U plane data
 * @param v      V plane data
 * @param y_stride Y plane stride (bytes per row)
 * @param u_stride U plane stride
 * @param v_stride V plane stride
 * @return 0 on success, -1 on error
 */
int y4m_writer_write_frame(Y4MWriter *writer,
                            const uint8_t *y,
                            const uint8_t *u,
                            const uint8_t *v,
                            int y_stride,
                            int u_stride,
                            int v_stride);

/**
 * Write a frame from an Av1OutputBuffer.
 * 
 * @param writer Writer handle
 * @param buffer Output buffer with frame data
 * @return 0 on success, -1 on error
 */
int y4m_writer_write_buffer(Y4MWriter *writer, 
                             const Av1OutputBuffer *buffer);

/**
 * Get the number of frames written.
 * 
 * @param writer Writer handle
 * @return Number of frames written
 */
uint64_t y4m_writer_get_frame_count(const Y4MWriter *writer);

/**
 * Check if the writer is valid and open.
 * 
 * @param writer Writer handle
 * @return true if valid, false otherwise
 */
bool y4m_writer_is_valid(const Y4MWriter *writer);

#ifdef __cplusplus
}
#endif

#endif // Y4M_WRITER_H
