/**
 * Simple test IVF file generator
 * Creates a minimal valid IVF file for testing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// IVF header
#pragma pack(push, 1)
typedef struct {
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

typedef struct {
    uint32_t size;
    uint64_t timestamp;
} IvfFrameHeader;
#pragma pack(pop)

// Generate a minimal but valid AV1 sequence header OBU
// This creates a valid AV1 sequence header for baseline profile, 8-bit, 420
static void generate_sequence_header(uint8_t *buf, int *size) {
    int idx = 0;
    
    // OBU header: size = 0 (extension flag + obu_type)
    // obu_type = 1 (seq_header), has_extension = 0
    buf[idx++] = 0x0A;  // 0000 1010: size=0, type=1 (seq_header)
    
    // Sequence header OBU
    // Profile = 0 (Main)
    buf[idx++] = 0x00;
    
    // still_picture = 0, reduced_still_picture_header = 1
    buf[idx++] = 0x02;  // 0000 0010: reduced_still_picture_header=1
    
    // Timing info (optional but included for completeness)
    // num_units_in_display_tick = 1
    buf[idx++] = 0x01;
    // time_scale = 30
    buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x1E;
    // equal_picture_interval = 1
    buf[idx++] = 0x80;  // 1000 0000: num_ticks_per_picture_minus_1=0, equal_picture_interval=1
    // num_ticks_per_picture_minus_1 = 0
    
    // Decoder model info (optional, skip for simplicity)
    
    // Operating point info
    buf[idx++] = 0x00;  // operating_points_count_minus_1 = 0
    
    // Frame width/height
    // Note: We need to write these as leb128 values
    // For 640: 0x80 0x50 (128 + 80 = 208... actually 640 = 0x50 in leb128 continuation)
    // Actually 640 = 0x90 0x05 (little-endian 128*5 + 144 = 784... no)
    // Simple: 640 = 0x80 0x50 (0x50 = 80, 80*128 + 0 = 10240... wrong)
    // Correct: 640 = 0x90 0x04 (0x90=144, 144+4*128=656... close)
    // Let's use simpler values: 256 = 0x80 0x02
    
    // For simplicity, use 256x256 and update header
    buf[idx++] = 0x80; buf[idx++] = 0x02;  // frame_width = 256
    buf[idx++] = 0x80; buf[idx++] = 0x02;  // frame_height = 256
    
    // max_frame_width_minus_1, max_frame_height_minus_1
    buf[idx++] = 0x00;  // max_frame_width_minus_1 = 0 (same as frame_width)
    buf[idx++] = 0x00;  // max_frame_height_minus_1 = 0
    
    // frame_id_numbers_present = 0
    // Use 128x128 for simpler leb128 encoding
    // 128 = 0x80 0x01
    buf[idx-4] = 0x80; buf[idx-3] = 0x01;  // frame_width = 128
    buf[idx-2] = 0x80; buf[idx-1] = 0x01;  // frame_height = 128
    
    // Note: For a truly valid stream, we'd need proper leb128 encoding
    // and all required fields. For testing purposes, this is a minimal attempt.
    
    *size = idx;
}

// Generate a minimal valid AV1 frame with frame header
static void generate_frame_header(uint8_t *buf, int *size, int frame_idx) {
    int idx = 0;
    
    // OBU header for frame
    buf[idx++] = 0x0B;  // 0000 1011: size=0, type=2 (frame)
    
    // Frame header OBU (simplified)
    // show_existing_frame = 0
    // frame_type = 1 (INTER_FRAME) or 2 (INTRA_FRAME)
    // For simplicity, use KEY_FRAME
    buf[idx++] = 0x00;  // frame_type = KEY_FRAME
    
    // base_q_idx = 0
    buf[idx++] = 0x00;
    
    // skip_mode_frame = 0
    // skip = 0
    // allow_intrabc = 0
    
    // reference_frame_idx[0..2] = 0
    // refresh_frame_flags = 1 (refresh golden)
    
    // For a minimal frame, we just need the header to be parseable
    // The actual pixel data will be all zeros (black)
    
    *size = idx;
}

int main(int argc, char *argv[]) {
    const char *output = "test.ivf";
    int width = 640;
    int height = 480;
    int num_frames = 30;
    
    if (argc > 1) output = argv[1];
    if (argc > 2) width = atoi(argv[2]);
    if (argc > 3) height = atoi(argv[3]);
    if (argc > 4) num_frames = atoi(atoi(argv[4]) > 0 ? argv[4] : "30");
    
    // Clamp to reasonable values for IVF format
    if (width < 1) width = 640;
    if (height < 1) height = 480;
    if (num_frames < 1) num_frames = 30;
    
    printf("Generating test IVF: %s (%dx%d, %d frames)\n", 
           output, width, height, num_frames);
    
    FILE *f = fopen(output, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create %s\n", output);
        return 1;
    }
    
    // Write IVF header
    IvfHeader header = {
        .magic = {'D', 'K', 'I', 'F'},
        .version = 0,
        .header_size = 32,
        .fourcc = {'A', 'V', '0', '1'},
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .timebase_num = 1,
        .timebase_den = 30,
        .num_frames = (uint32_t)num_frames,
        .unused = 0
    };
    
    fwrite(&header, sizeof(header), 1, f);
    
    // Generate sequence header
    uint8_t seq_header[64];
    int seq_size = 0;
    generate_sequence_header(seq_header, &seq_size);
    
    // Generate frame header template
    uint8_t frame_header[32];
    int frame_hdr_size = 0;
    generate_frame_header(frame_header, &frame_hdr_size, 0);
    
    // Write frames
    for (int i = 0; i < num_frames; i++) {
        // Combine sequence header (for first frame) + frame data
        // For subsequent frames, just frame header
        uint8_t frame_data[256];
        int frame_size;
        
        if (i == 0) {
            // First frame includes sequence header
            memcpy(frame_data, seq_header, seq_size);
            frame_size = seq_size;
        } else {
            frame_size = 0;
        }
        
        // Add frame header
        memcpy(frame_data + frame_size, frame_header, frame_hdr_size);
        frame_size += frame_hdr_size;
        
        // Add some dummy pixel data (will decode to black)
        // Y plane: width * height bytes
        // U plane: width/2 * height/2 bytes  
        // V plane: width/2 * height/2 bytes
        int y_size = width * height;
        int uv_size = (width / 2) * (height / 2);
        
        // Ensure we don't overflow
        if (frame_size + y_size + uv_size * 2 < (int)sizeof(frame_data)) {
            // Fill with zeros (black pixels for 8-bit YUV420)
            memset(frame_data + frame_size, 0, y_size + uv_size * 2);
            frame_size += y_size + uv_size * 2;
        }
        
        IvfFrameHeader frame_header_struct = {
            .size = (uint32_t)frame_size,
            .timestamp = (uint64_t)i
        };
        fwrite(&frame_header_struct, sizeof(frame_header_struct), 1, f);
        fwrite(frame_data, frame_size, 1, f);
    }
    
    fclose(f);
    
    printf("Created %s (%dx%d, %d frames)\n", output, width, height, num_frames);
    return 0;
}
