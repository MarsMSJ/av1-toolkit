#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

int main(int argc, char *argv[]) {
    const char *output = "test.ivf";
    int width = 640;
    int height = 480;
    int num_frames = 30;
    
    if (argc > 1) output = argv[1];
    if (argc > 2) width = atoi(argv[2]);
    if (argc > 3) height = atoi(argv[3]);
    if (argc > 4) num_frames = atoi(argv[4]);
    
    printf("Generating test IVF: %s (%dx%d, %d frames)\n", 
           output, width, height, num_frames);
    
    FILE *f = fopen(output, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create %s\n", output);
        return 1;
    }
    
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
    
    uint8_t minimal_frame[] = { 0x00, 0x00, 0x00 };
    
    for (int i = 0; i < num_frames; i++) {
        IvfFrameHeader frame_header = {
            .size = sizeof(minimal_frame),
            .timestamp = (uint64_t)i
        };
        fwrite(&frame_header, sizeof(frame_header), 1, f);
        fwrite(minimal_frame, sizeof(minimal_frame), 1, f);
    }
    
    fclose(f);
    printf("Created %s\n", output);
    return 0;
}
