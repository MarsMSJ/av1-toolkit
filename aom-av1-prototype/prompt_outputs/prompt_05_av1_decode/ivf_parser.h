#ifndef IVF_PARSER_H
#define IVF_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// IVF File Format Structures
// ============================================================================

// IVF file header (32 bytes)
typedef struct __attribute__((packed)) IvfHeader {
    char     magic[4];         // "DKIF"
    uint16_t version;          // Usually 0
    uint16_t header_size;      // Usually 32
    char     fourcc[4];        // FourCC code (e.g., "AV01")
    uint16_t width;            // Frame width
    uint16_t height;           // Frame height
    uint32_t timebase_num;     // Numerator of timebase
    uint32_t timebase_den;     // Denominator of timebase
    uint32_t num_frames;       // Number of frames in file
    uint32_t unused;           // Unused (usually 0)
} IvfHeader;

// IVF frame header (12 bytes)
typedef struct __attribute__((packed)) IvfFrameHeader {
    uint32_t size;             // Size of frame data in bytes
    uint64_t timestamp;        // Presentation timestamp in timebase units
} IvfFrameHeader;

// ============================================================================
// Parser Handle
// ============================================================================

typedef struct IvfParser IvfParser;

// ============================================================================
// API Functions
// ============================================================================

/**
 * Open an IVF file for reading.
 * 
 * @param filename Path to IVF file
 * @return Parser handle on success, NULL on failure
 */
IvfParser *ivf_parser_open(const char *filename);

/**
 * Close an IVF file and free resources.
 * 
 * @param parser Parser handle
 */
void ivf_parser_close(IvfParser *parser);

/**
 * Get the IVF file header information.
 * 
 * @param parser Parser handle
 * @return Pointer to header (valid until parser is closed), or NULL on error
 */
const IvfHeader *ivf_parser_get_header(const IvfParser *parser);

/**
 * Read the next frame from the IVF file.
 * 
 * @param parser   Parser handle
 * @param out_data Output buffer for frame data (caller must free with free())
 * @param out_size Output size of frame data in bytes
 * @param out_timestamp Output timestamp in timebase units
 * @return 0 on success, -1 on end of file or error
 */
int ivf_parser_read_frame(IvfParser *parser, 
                          uint8_t **out_data, 
                          size_t *out_size,
                          uint64_t *out_timestamp);

/**
 * Check if we've reached end of file.
 * 
 * @param parser Parser handle
 * @return true if at EOF, false otherwise
 */
bool ivf_parser_eof(const IvfParser *parser);

/**
 * Get current frame index.
 * 
 * @param parser Parser handle
 * @return Current frame index (0-based), or -1 on error
 */
int ivf_parser_get_frame_index(const IvfParser *parser);

/**
 * Get total number of frames in the file.
 * 
 * @param parser Parser handle
 * @return Number of frames, or -1 on error
 */
int ivf_parser_get_num_frames(const IvfParser *parser);

/**
 * Seek to a specific frame.
 * 
 * @param parser Parser handle
 * @param frame_index Frame index to seek to (0-based)
 * @return 0 on success, -1 on error
 */
int ivf_parser_seek_frame(IvfParser *parser, int frame_index);

/**
 * Check if the file is a valid IVF file.
 * 
 * @param filename Path to file
 * @return true if valid IVF, false otherwise
 */
bool ivf_parser_is_valid(const char *filename);

#ifdef __cplusplus
}
#endif

#endif // IVF_PARSER_H
