

## Progress
- [x] y4m_writer — checked, 2 bugs found

## Focus area 1: y4m_writer.c
### Bug 1.1: Missing "Ip" progressive flag in Y4M header
- **File**: y4m_writer.c, line ~60
- **Problem**: The Y4M header format is missing the "Ip" progressive scan flag. The spec requires 'YUV4MPEG2 W%d H%d F%d:%d Ip C%s\n' for progressive video, which is the most common format. Without "Ip", some players may not interpret the video correctly.
- **Fix**: Add "Ip" to the fprintf format string.

### Bug 1.2: Incorrect bit depth suffix for 12-bit and 16-bit video
- **File**: y4m_writer.c, lines ~26-40 (get_colorspace function)
- **Problem**: The function only handles 8-bit and "p10" (10-bit). For any bit depth > 8, it always returns "p10" regardless of whether the actual bit depth is 10, 12, or 16. This means 12-bit video would incorrectly be labeled as "C420p10" instead of "C420p12", and 16-bit as "C420p10" instead of "C420p16".
- **Fix**: Extend the function to handle bit depths 10, 12, and 16 with their respective suffixes.

---

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
    if (bit_depth <= 8) {
        switch (chroma_subsampling) {
            case 0: return "420";
            case 1: return "422";
            case 2: return "444";
            default: return "420";
        }
    } else if (bit_depth <= 10) {
        switch (chroma_subsampling) {
            case 0: return "420p10";
            case 1: return "422p10";
            case 2: return "444p10";
            default: return "420p10";
        }
    } else if (bit_depth <= 12) {
        switch (chroma_subsampling) {
            case 0: return "420p12";
            case 1: return "422p12";
            case 2: return "444p12";
            default: return "420p12";
        }
    } else {
        /* bit_depth > 12: assume 16-bit */
        switch (chroma_subsampling) {
            case 0: return "420p16";
            case 1: return "422p16";
            case 2: return "444p16";
            default: return "420p16";
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
    fprintf(writer->file, "YUV4MPEG2 W%d H%d F%d:%d Ip C%s\n",
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
        case 0:
            uv_width = writer->width / 2;
            uv_height = writer->height / 2;
            break;
        case 1:
            uv_width = writer->width / 2;
            break;
        case 2:
            break;
        default:
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
    if (!writer || !buffer || !buffer->planes[0]) {
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