

## Code Review: y4m_writer.c

I've identified several bugs in the code:

### Bug 1: y4m_writer.h - Missing Av1OutputBuffer definition
**File:** y4m_writer.h  
**Line:** 14 (function declaration)  
**Problem:** The header declares `y4m_writer_write_buffer` with `struct Av1OutputBuffer *buffer` but never includes the header that defines this struct or forward-declares it. This will cause a compile error when this header is used.

### Bug 2: y4m_writer.c - Uninitialized uv_height for 4:2:2
**File:** y4m_writer.c  
**Line:** ~93-95  
**Problem:** For chroma_subsampling case 1 (4:2:2), the code sets `uv_width` but never sets `uv_height`. The variable `uv_height` remains uninitialized (or retains garbage from the earlier assignment), leading to incorrect frame output or memory issues.

### Bug 3: y4m_writer.c - 12-bit colorspace not handled
**File:** y4m_writer.c  
**Line:** ~30-40  
**Problem:** The `get_colorspace` function only checks `if (bit_depth > 8)` and returns "p10" suffix for any bit depth > 8. For 12-bit content, the colorspace should be "420p12", "422p12", "444p12", not "420p10", etc.

### Bug 4: y4m_writer.c - Insufficient buffer validation
**File:** y4m_writer.c  
**Line:** ~127-130  
**Problem:** `y4m_writer_write_buffer` only checks if `planes[0]` is non-NULL, but doesn't check `planes[1]` or `planes[2]`. If the buffer has valid Y plane but invalid U/V planes, the subsequent call to `y4m_writer_write_frame` will crash or return errors.

---

### Corrected Files

### y4m_writer.h
```c
#ifndef Y4M_WRITER_H
#define Y4M_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Forward declaration - actual definition must be provided by av1_decoder_api.h */
struct Av1OutputBuffer;

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
```

### y4m_writer.c
```c
#include "y4m_writer.h"
#include "av1_decoder_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct Y4MWriter {
    FILE *file;
    int width;
    int height;
    int fps_n;
    int fps_d;
    int bit_depth;
    int chroma_subsampling;
    uint64_t frame_count;
    bool valid;
};

static const char* get_colorspace(int chroma_subsampling, int bit_depth) {
    if (bit_depth == 10) {
        switch (chroma_subsampling) {
            case 0: return "420p10";
            case 1: return "422p10";
            case 2: return "444p10";
            default: return "420p10";
        }
    } else if (bit_depth == 12) {
        switch (chroma_subsampling) {
            case 0: return "420p12";
            case 1: return "422p12";
            case 2: return "444p12";
            default: return "420p12";
        }
    } else if (bit_depth > 8) {
        /* Default to 10-bit for unknown > 8 bit depths for compatibility */
        switch (chroma_subsampling) {
            case 0: return "420p10";
            case 1: return "422p10";
            case 2: return "444p10";
            default: return "420p10";
        }
    } else {
        switch (chroma_subsampling) {
            case 0: return "420";
            case 1: return "422";
            case 2: return "444";
            default: return "420";
        }
    }
}

Y4MWriter *y4m_writer_open(const char *filename, int width, int height,
                            int fps_n, int fps_d, int bit_depth, int chroma_subsampling) {
    if (!filename || width <= 0 || height <= 0) {
        fprintf(stderr, "y4m_writer_open: invalid parameters\n");
        return NULL;
    }
    
    Y4MWriter *writer = (Y4MWriter *)calloc(1, sizeof(Y4MWriter));
    if (!writer) {
        fprintf(stderr, "y4m_writer_open: failed to allocate writer\n");
        return NULL;
    }
    
    writer->file = fopen(filename, "wb");
    if (!writer->file) {
        fprintf(stderr, "y4m_writer_open: failed to open file: %s\n", filename);
        free(writer);
        return NULL;
    }
    
    writer->width = width;
    writer->height = height;
    writer->fps_n = fps_n > 0 ? fps_n : 30;
    writer->fps_d = fps_d > 0 ? fps_d : 1;
    writer->bit_depth = bit_depth > 0 ? bit_depth : 8;
    writer->chroma_subsampling = chroma_subsampling;
    writer->frame_count = 0;
    writer->valid = true;
    
    const char *colorspace = get_colorspace(chroma_subsampling, writer->bit_depth);
    fprintf(writer->file, "YUV4MPEG2 W%d H%d F%d:%d C%s\n",
            width, height, writer->fps_n, writer->fps_d, colorspace);
    
    return writer;
}

int y4m_writer_close(Y4MWriter *writer) {
    if (!writer) {
        return -1;
    }
    
    if (writer->file) {
        fclose(writer->file);
    }
    
    free(writer);
    return 0;
}

int y4m_writer_write_frame(Y4MWriter *writer, const uint8_t *y, const uint8_t *u, const uint8_t *v,
                            int y_stride, int u_stride, int v_stride) {
    if (!writer || !writer->valid || !y || !u || !v) {
        return -1;
    }
    
    fprintf(writer->file, "FRAME\n");
    
    int y_width = writer->width;
    int y_height = writer->height;
    int uv_width = writer->width;
    int uv_height = writer->height;
    
    switch (writer->chroma_subsampling) {
        case 0:  /* 4:2:0 */
            uv_width = writer->width / 2;
            uv_height = writer->height / 2;
            break;
        case 1:  /* 4:2:2 */
            uv_width = writer->width / 2;
            uv_height = writer->height;  /* Fixed: 4:2:2 has full height */
            break;
        case 2:  /* 4:4:4 */
            uv_width = writer->width;
            uv_height = writer->height;
            break;
        default:  /* Default to 4:2:0 */
            uv_width = writer->width / 2;
            uv_height = writer->height / 2;
            break;
    }
    
    int bytes_per_sample = (writer->bit_depth > 8) ? 2 : 1;
    
    for (int row = 0; row < y_height; row++) {
        const uint8_t *row_data = y + (row * y_stride);
        if (fwrite(row_data, bytes_per_sample, y_width, writer->file) != (size_t)y_width) {
            return -1;
        }
    }
    
    for (int row = 0; row < uv_height; row++) {
        const uint8_t *row_data = u + (row * u_stride);
        if (fwrite(row_data, bytes_per_sample, uv_width, writer->file) != (size_t)uv_width) {
            return -1;
        }
    }
    
    for (int row = 0; row < uv_height; row++) {
        const uint8_t *row_data = v + (row * v_stride);
        if (fwrite(row_data, bytes_per_sample, uv_width, writer->file) != (size_t)uv_width) {
            return -1;
        }
    }
    
    writer->frame_count++;
    
    return 0;
}

int y4m_writer_write_buffer(Y4MWriter *writer, const Av1OutputBuffer *buffer) {
    if (!writer || !buffer || !buffer->planes[0] || !buffer->planes[1] || !buffer->planes[2]) {
        return -1;
    }
    
    return y4m_writer_write_frame(writer, buffer->planes[0], buffer->planes[1], buffer->planes[2],
                                   buffer->strides[0], buffer->strides[1], buffer->strides[2]);
}

uint64_t y4m_writer_get_frame_count(const Y4MWriter *writer) {
    if (!writer) {
        return 0;
    }
    return writer->frame_count;
}

bool y4m_writer_is_valid(const Y4MWriter *writer) {
    return writer && writer->valid;
}
```