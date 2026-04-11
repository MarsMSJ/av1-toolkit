#ifndef IVF_PARSER_H
#define IVF_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) IvfHeader {
    char     magic[4];
    uint16_t version;
    uint16_t header_size;
    char     fourcc[4];
    uint16_t width;
    uint16_t height;
    uint32_t timebase_num;
    uint32_t timebase_den;
    uint32_t num_frames;
    uint32_t unused;
} IvfHeader;

typedef struct __attribute__((packed)) IvfFrameHeader {
    uint32_t size;
    uint64_t timestamp;
} IvfFrameHeader;

typedef struct IvfParser IvfParser;

IvfParser *ivf_parser_open(const char *filename);
void ivf_parser_close(IvfParser *parser);
const IvfHeader *ivf_parser_get_header(const IvfParser *parser);
int ivf_parser_read_frame(IvfParser *parser, uint8_t **out_data, size_t *out_size, uint64_t *out_timestamp);
bool ivf_parser_eof(const IvfParser *parser);
int ivf_parser_get_frame_index(const IvfParser *parser);
int ivf_parser_get_num_frames(const IvfParser *parser);
int ivf_parser_seek_frame(IvfParser *parser, int frame_index);
bool ivf_parser_is_valid(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* IVF_PARSER_H */
