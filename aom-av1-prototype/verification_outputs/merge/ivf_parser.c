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

static void read_header(IvfParser *parser) {
    if (!parser || !parser->file) {
        return;
    }
    
    fseek(parser->file, 0, SEEK_SET);
    size_t read = fread(&parser->header, sizeof(IvfHeader), 1, parser->file);
    
    if (read != 1) {
        parser->valid = false;
        return;
    }
    
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
    
    IvfFrameHeader frame_header;
    size_t read = fread(&frame_header, sizeof(IvfFrameHeader), 1, parser->file);
    
    if (read != 1) {
        parser->eof = true;
        return -1;
    }
    
    uint8_t *data = (uint8_t *)malloc(frame_header.size);
    if (!data) {
        fprintf(stderr, "ivf_parser_read_frame: failed to allocate frame buffer\n");
        return -1;
    }
    
    read = fread(data, 1, frame_header.size, parser->file);
    
    if (read != frame_header.size) {
        fprintf(stderr, "ivf_parser_read_frame: failed to read frame data\n");
        free(data);
        return -1;
    }
    
    *out_data = data;
    *out_size = frame_header.size;
    
    if (out_timestamp) {
        *out_timestamp = frame_header.timestamp;
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
        IvfFrameHeader frame_header;
        if (fread(&frame_header, sizeof(IvfFrameHeader), 1, parser->file) != 1) {
            return -1;
        }
        fseek(parser->file, frame_header.size, SEEK_CUR);
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
