

## Progress
- [x] ivf_parser — checked, 5 bugs found

## Focus area 1: ivf_parser

### Bug 1.1: Missing endianness conversion for header fields
- **File**: ivf_parser.c, line ~27
- **Problem**: The `IvfHeader` struct is read directly from file using `fread`. IVF files store multi-byte fields in little-endian format. On big-endian systems, `version`, `header_size`, `width`, `height`, `timebase_num`, `timebase_den`, and `num_frames` will be read incorrectly.
- **Fix**: Use explicit little-endian read functions (e.g., `read_le16`, `read_le32`) or byte-swap after reading.

### Bug 1.2: Missing endianness conversion for frame header fields
- **File**: ivf_parser.c, line ~93
- **Problem**: `IvfFrameHeader` is read directly with `fread`. The `size` (uint32_t) and `timestamp` (uint64_t) fields are little-endian in IVF files. On big-endian systems, these will be read incorrectly, causing wrong frame sizes and timestamps.
- **Fix**: Use explicit little-endian read functions for `size` and `timestamp`.

### Bug 1.3: Fourcc field not validated
- **File**: ivf_parser.c, line ~38-48
- **Problem**: The `read_header()` function validates the magic number but never checks the `fourcc` field to verify the file contains AV1 data ('AV01'). A file with wrong fourcc would be accepted as valid.
- **Fix**: Add check for `fourcc[0]=='A' && fourcc[1]=='V' && fourcc[2]=='0' && fourcc[3]=='1'`.

### Bug 1.4: No bounds check on frame size before allocation
- **File**: ivf_parser.c, line ~97
- **Problem**: `malloc(frame_header.size)` is called without validating that `frame_header.size` is reasonable. A malicious or corrupted IVF file could specify an enormous size, causing malloc to fail or return a small buffer that gets overflowed.
- **Fix**: Add sanity check on frame size (e.g., max frame size like 8MB or 16MB for AV1).

### Bug 1.5: No bounds check before reading frame data
- **File**: ivf_parser.c, line ~101-106
- **Problem**: The code reads `frame_header.size` bytes without checking if the file actually contains that many bytes. If the file is truncated, `fread` may read fewer bytes than requested but still return a partial buffer.
- **Fix**: Check that `fread` returns exactly `frame_header.size` bytes, and verify file has enough data before reading.

---

### ivf_parser.c

```c
#include "ivf_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

struct IvfParser {
    FILE *file;
    IvfHeader header;
    int current_frame;
    bool eof;
    bool valid;
};

#define IVF_MAGIC "DKIF"
#define IVF_FOURCC_AV1 "AV01"
#define IVF_MAX_FRAME_SIZE (16 * 1024 * 1024)  /* 16MB max frame size */

/* Little-endian read helpers for portable big-endian support */
static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t *data) {
    return (uint64_t)data[0] | ((uint64_t)data[1] << 8) |
           ((uint64_t)data[2] << 16) | ((uint64_t)data[3] << 24) |
           ((uint64_t)data[4] << 32) | ((uint64_t)data[5] << 40) |
           ((uint64_t)data[6] << 48) | ((uint64_t)data[7] << 56);
}

static void read_header(IvfParser *parser) {
    if (!parser || !parser->file) {
        return;
    }
    
    /* Read raw header bytes for endian-safe parsing */
    uint8_t raw_header[32];
    fseek(parser->file, 0, SEEK_SET);
    size_t read = fread(raw_header, 1, 32, parser->file);
    
    if (read != 32) {
        parser->valid = false;
        return;
    }
    
    /* Parse header fields as little-endian */
    memcpy(parser->header.magic, raw_header, 4);
    parser->header.version = read_le16(raw_header + 4);
    parser->header.header_size = read_le16(raw_header + 6);
    memcpy(parser->header.fourcc, raw_header + 8, 4);
    parser->header.width = read_le16(raw_header + 12);
    parser->header.height = read_le16(raw_header + 14);
    parser->header.timebase_num = read_le32(raw_header + 16);
    parser->header.timebase_den = read_le32(raw_header + 20);
    parser->header.num_frames = read_le32(raw_header + 24);
    /* raw_header[28-31] is unused */
    
    if (memcmp(parser->header.magic, IVF_MAGIC, 4) != 0) {
        fprintf(stderr, "Invalid IVF magic: %.4s\n", parser->header.magic);
        parser->valid = false;
        return;
    }
    
    if (parser->header.version != 0) {
        fprintf(stderr, "Unsupported IVF version: %d\n", parser->header.version);
        parser->valid = false;
        return;
    }
    
    if (parser->header.header_size != 32) {
        fprintf(stderr, "Unexpected IVF header size: %d\n", parser->header.header_size);
        parser->valid = false;
        return;
    }
    
    /* Validate fourcc is AV01 */
    if (memcmp(parser->header.fourcc, IVF_FOURCC_AV1, 4) != 0) {
        fprintf(stderr, "Invalid IVF fourcc: %.4s (expected AV01)\n", parser->header.fourcc);
        parser->valid = false;
        return;
    }
    
    parser->valid = true;
    parser->current_frame = 0;
    parser->eof = false;
}

IvfParser *ivf_parser_open(const char *filename) {
    if (!filename) {
        fprintf(stderr, "ivf_parser_open: NULL filename\n");
        return NULL;
    }
    
    IvfParser *parser = (IvfParser *)calloc(1, sizeof(IvfParser));
    if (!parser) {
        fprintf(stderr, "ivf_parser_open: failed to allocate parser\n");
        return NULL;
    }
    
    parser->file = fopen(filename, "rb");
    if (!parser->file) {
        fprintf(stderr, "ivf_parser_open: failed to open file: %s\n", filename);
        free(parser);
        return NULL;
    }
    
    read_header(parser);
    
    if (!parser->valid) {
        fprintf(stderr, "ivf_parser_open: invalid IVF file: %s\n", filename);
        fclose(parser->file);
        free(parser);
        return NULL;
    }
    
    return parser;
}

void ivf_parser_close(IvfParser *parser) {
    if (!parser) {
        return;
    }
    
    if (parser->file) {
        fclose(parser->file);
    }
    
    free(parser);
}

const IvfHeader *ivf_parser_get_header(const IvfParser *parser) {
    if (!parser || !parser->valid) {
        return NULL;
    }
    
    return &parser->header;
}

int ivf_parser_read_frame(IvfParser *parser, uint8_t **out_data, size_t *out_size, uint64_t *out_timestamp) {
    if (!parser || !out_data || !out_size) {
        return -1;
    }
    
    if (!parser->valid || parser->eof) {
        parser->eof = true;
        return -1;
    }
    
    /* Read raw frame header bytes for endian-safe parsing */
    uint8_t raw_frame_header[12];
    size_t read = fread(raw_frame_header, 1, 12, parser->file);
    
    if (read != 12) {
        parser->eof = true;
        return -1;
    }
    
    uint32_t frame_size = read_le32(raw_frame_header);
    uint64_t timestamp = read_le64(raw_frame_header + 4);
    
    /* Validate frame size is reasonable */
    if (frame_size == 0 || frame_size > IVF_MAX_FRAME_SIZE) {
        fprintf(stderr, "ivf_parser_read_frame: invalid frame size %u\n", frame_size);
        parser->eof = true;
        return -1;
    }
    
    uint8_t *data = (uint8_t *)malloc(frame_size);
    if (!data) {
        fprintf(stderr, "ivf_parser_read_frame: failed to allocate frame buffer\n");
        return -1;
    }
    
    /* Read exactly frame_size bytes and verify success */
    read = fread(data, 1, frame_size, parser->file);
    
    if (read != frame_size) {
        fprintf(stderr, "ivf_parser_read_frame: failed to read frame data (expected %u, got %zu)\n", frame_size, read);
        free(data);
        parser->eof = true;
        return -1;
    }
    
    *out_data = data;
    *out_size = frame_size;
    
    if (out_timestamp) {
        *out_timestamp = timestamp;
    }
    
    parser->current_frame++;
    
    if (feof(parser->file)) {
        parser->eof = true;
    }
    
    return 0;
}

bool ivf_parser_eof(const IvfParser *parser) {
    if (!parser) {
        return true;
    }
    return parser->eof;
}

int ivf_parser_get_frame_index(const IvfParser *parser) {
    if (!parser) {
        return -1;
    }
    return parser->current_frame;
}

int ivf_parser_get_num_frames(const IvfParser *parser) {
    if (!parser || !parser->valid) {
        return -1;
    }
    return (int)parser->header.num_frames;
}

int ivf_parser_seek_frame(IvfParser *parser, int frame_index) {
    if (!parser || !parser->valid) {
        return -1;
    }
    
    if (frame_index < 0 || frame_index >= (int)parser->header.num_frames) {
        fprintf(stderr, "ivf_parser_seek_frame: invalid frame index %d\n", frame_index);
        return -1;
    }
    
    fseek(parser->file, 32, SEEK_SET);  /* Skip IVF header (32 bytes) */
    
    for (int i = 0; i < frame_index; i++) {
        /* Read raw frame header for endian-safe parsing */
        uint8_t raw_frame_header[12];
        if (fread(raw_frame_header, 1, 12, parser->file) != 12) {
            return -1;
        }
        
        uint32_t frame_size = read_le32(raw_frame_header);
        
        /* Validate frame size before seeking */
        if (frame_size > IVF_MAX_FRAME_SIZE) {
            fprintf(stderr, "ivf_parser_seek_frame: invalid frame size %u at index %d\n", frame_size, i);
            return -1;
        }
        
        if (fseek(parser->file, frame_size, SEEK_CUR) != 0) {
            return -1;
        }
    }
    
    parser->current_frame = frame_index;
    parser->eof = false;
    
    return 0;
}

bool ivf_parser_is_valid(const char *filename) {
    if (!filename) {
        return false;
    }
    
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return false;
    }
    
    char magic[4];
    size_t read = fread(magic, 1, 4, file);
    fclose(file);
    
    if (read != 4) {
        return false;
    }
    
    return memcmp(magic, IVF_MAGIC, 4) == 0;
}
```