

Looking at this code, I need to analyze it for the specific issues mentioned: endianness, buffer bounds, fourcc validation, and frame iteration.

## Bug Analysis

### 1. Endianness Issues (Critical)

**File:** `ivf_parser.c`

The IVF format stores all multi-byte integers in little-endian format. The code uses raw `fread` into structs, which only works correctly on little-endian systems. On big-endian systems, all multi-byte fields will be read incorrectly.

- **Lines ~18-35 (`read_header`)**: Reads `IvfHeader` struct directly - `version`, `header_size`, `width`, `height`, `timebase_num`, `timebase_den`, `num_frames` all need byte-swapping on big-endian.
- **Lines ~89-91 (`ivf_parser_read_frame`)**: Reads `IvfFrameHeader` directly - `size` (uint32_t) and `timestamp` (uint64_t) need byte-swapping.
- **Lines ~161-167 (`ivf_parser_seek_frame`)**: Same issue when reading frame headers during seek.

### 2. Missing FourCC Validation (Bug)

**File:** `ivf_parser.c`, line ~30-35

The code reads the `fourcc` field but never validates it contains 'AV01'. This allows non-AV1 IVF files to be opened, which will cause failures later when passed to the AOM decoder.

### 3. Incomplete EOF Handling on Read Error

**File:** `ivf_parser.c`, lines ~103-107

When `fread` fails to read the full frame data, the code reports an error and frees the buffer, but doesn't set `parser->eof = true`. This leaves the parser in an inconsistent state.

### 4. Missing EOF Reset in Seek

**File:** `ivf_parser.c`, line ~175

After seeking, `parser->eof` is set to `false`, which is actually correct here. Let me re-check... Yes, line 175 does set `parser->eof = false`. This is fine.

### 5. Buffer Bounds Not Checked

**File:** `ivf_parser.c`, lines ~93-107

When reading frame data, if the file is truncated, `fread` may read fewer bytes than `frame_header.size`. The code detects this but doesn't verify the file has enough data before attempting to read. This could cause undefined behavior if a malicious or corrupted file specifies an enormous frame size.

---

## Corrected Files

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

/* Read a little-endian 16-bit value */
static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/* Read a little-endian 32-bit value */
static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/* Read a little-endian 64-bit value */
static uint64_t read_le64(const uint8_t *data) {
    return (uint64_t)data[0] |
           ((uint64_t)data[1] << 8) |
           ((uint64_t)data[2] << 16) |
           ((uint64_t)data[3] << 24) |
           ((uint64_t)data[4] << 32) |
           ((uint64_t)data[5] << 40) |
           ((uint64_t)data[6] << 48) |
           ((uint64_t)data[7] << 56);
}

static void read_header(IvfParser *parser) {
    if (!parser || !parser->file) {
        return;
    }
    
    uint8_t header_bytes[sizeof(IvfHeader)];
    
    fseek(parser->file, 0, SEEK_SET);
    size_t read = fread(header_bytes, sizeof(IvfHeader), 1, parser->file);
    
    if (read != 1) {
        parser->valid = false;
        return;
    }
    
    /* Read magic (4 bytes) - no endian conversion needed for char array */
    memcpy(parser->header.magic, header_bytes, 4);
    
    if (memcmp(parser->header.magic, IVF_MAGIC, 4) != 0) {
        fprintf(stderr, "Invalid IVF magic: %.4s\n", parser->header.magic);
        parser->valid = false;
        return;
    }
    
    /* Read version (uint16_t little-endian) */
    parser->header.version = read_le16(header_bytes + 4);
    
    if (parser->header.version != 0) {
        fprintf(stderr, "Unsupported IVF version: %d\n", parser->header.version);
        parser->valid = false;
        return;
    }
    
    /* Read header_size (uint16_t little-endian) */
    parser->header.header_size = read_le16(header_bytes + 6);
    
    if (parser->header.header_size != 32) {
        fprintf(stderr, "Unexpected IVF header size: %d\n", parser->header.header_size);
        parser->valid = false;
        return;
    }
    
    /* Read fourcc (4 bytes) - no endian conversion needed for char array */
    memcpy(parser->header.fourcc, header_bytes + 8, 4);
    
    /* Validate fourcc is AV01 */
    if (memcmp(parser->header.fourcc, IVF_FOURCC_AV1, 4) != 0) {
        fprintf(stderr, "Invalid IVF fourcc: %.4s (expected AV01)\n", parser->header.fourcc);
        parser->valid = false;
        return;
    }
    
    /* Read width (uint16_t little-endian) */
    parser->header.width = read_le16(header_bytes + 12);
    
    /* Read height (uint16_t little-endian) */
    parser->header.height = read_le16(header_bytes + 14);
    
    /* Read timebase_num (uint32_t little-endian) */
    parser->header.timebase_num = read_le32(header_bytes + 16);
    
    /* Read timebase_den (uint32_t little-endian) */
    parser->header.timebase_den = read_le32(header_bytes + 20);
    
    /* Read num_frames (uint32_t little-endian) */
    parser->header.num_frames = read_le32(header_bytes + 24);
    
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
    
    /* Read frame header bytes (12 bytes: 4 for size, 8 for timestamp) */
    uint8_t frame_header_bytes[12];
    size_t read = fread(frame_header_bytes, 1, 12, parser->file);
    
    if (read != 12) {
        parser->eof = true;
        return -1;
    }
    
    /* Parse frame size and timestamp as little-endian */
    uint32_t frame_size = read_le32(frame_header_bytes);
    uint64_t timestamp = read_le64(frame_header_bytes + 4);
    
    /* Sanity check: reject unreasonably large frame sizes to prevent
       buffer allocation attacks or excessive memory usage */
    if (frame_size > 256 * 1024 * 1024) {  /* 256 MB limit */
        fprintf(stderr, "ivf_parser_read_frame: frame size too large: %u\n", frame_size);
        parser->eof = true;
        return -1;
    }
    
    uint8_t *data = (uint8_t *)malloc(frame_size);
    if (!data) {
        fprintf(stderr, "ivf_parser_read_frame: failed to allocate frame buffer\n");
        parser->eof = true;
        return -1;
    }
    
    read = fread(data, 1, frame_size, parser->file);
    
    if (read != frame_size) {
        fprintf(stderr, "ivf_parser_read_frame: failed to read frame data (expected %u, got %zu)\n",
                frame_size, read);
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
    
    fseek(parser->file, sizeof(IvfHeader), SEEK_SET);
    
    for (int i = 0; i < frame_index; i++) {
        /* Read frame header bytes (12 bytes) */
        uint8_t frame_header_bytes[12];
        if (fread(frame_header_bytes, 1, 12, parser->file) != 12) {
            return -1;
        }
        
        /* Parse frame size as little-endian */
        uint32_t frame_size = read_le32(frame_header_bytes);
        
        /* Sanity check on frame size during seek */
        if (frame_size > 256 * 1024 * 1024) {
            fprintf(stderr, "ivf_parser_seek_frame: invalid frame size: %u\n", frame_size);
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
    
    uint8_t header_bytes[32];
    size_t read = fread(header_bytes, 1, 32, file);
    fclose(file);
    
    if (read != 32) {
        return false;
    }
    
    /* Check magic */
    if (memcmp(header_bytes, IVF_MAGIC, 4) != 0) {
        return false;
    }
    
    /* Check version */
    uint16_t version = read_le16(header_bytes + 4);
    if (version != 0) {
        return false;
    }
    
    /* Check header_size */
    uint16_t header_size = read_le16(header_bytes + 6);
    if (header_size != 32) {
        return false;
    }
    
    /* Check fourcc is AV01 */
    if (memcmp(header_bytes + 8, IVF_FOURCC_AV1, 4) != 0) {
        return false;
    }
    
    return true;
}
```

### ivf_parser.h

This file has no bugs - it's a clean header file.