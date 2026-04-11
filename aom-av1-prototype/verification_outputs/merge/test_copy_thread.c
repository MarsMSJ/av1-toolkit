#include "av1_copy_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 1920
#define HEIGHT 1080
#define Y_SIZE (WIDTH * HEIGHT)
#define UV_SIZE (Y_SIZE / 4)

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while(0)

static void test_single_copy(void) {
    printf("Test 1: Basic single copy\n");
    
    uint8_t *src_y = aligned_alloc(32, Y_SIZE);
    uint8_t *src_u = aligned_alloc(32, UV_SIZE);
    uint8_t *src_v = aligned_alloc(32, UV_SIZE);
    uint8_t *dst_y = aligned_alloc(32, Y_SIZE);
    uint8_t *dst_u = aligned_alloc(32, UV_SIZE);
    uint8_t *dst_v = aligned_alloc(32, UV_SIZE);
    
    memset(src_y, 0xAA, Y_SIZE);
    memset(src_u, 0xBB, UV_SIZE);
    memset(src_v, 0xCC, UV_SIZE);
    memset(dst_y, 0, Y_SIZE);
    memset(dst_u, 0, UV_SIZE);
    memset(dst_v, 0, UV_SIZE);
    
    Av1CopyThread *ct = av1_copy_thread_create(4);
    ASSERT(ct != NULL, "Copy thread created");
    
    Av1CopyJob job = {
        .frame_id = 42,
        .dpb_slot = 0,
        .src_planes = { src_y, src_u, src_v },
        .src_strides = { WIDTH, WIDTH/2, WIDTH/2 },
        .dst_planes = { dst_y, dst_u, dst_v },
        .dst_strides = { WIDTH, WIDTH/2, WIDTH/2 },
        .plane_widths = { WIDTH, WIDTH/2, WIDTH/2 },
        .plane_heights = { HEIGHT, HEIGHT/2, HEIGHT/2 },
    };
    
    ASSERT(av1_copy_thread_enqueue(ct, &job) == 0, "Job enqueued");
    ASSERT(av1_copy_thread_wait(ct, &job, 0) == 0, "Job completed");
    ASSERT(av1_copy_thread_get_status(&job) == AV1_COPY_COMPLETE, "Status is COMPLETE");
    
    ASSERT(memcmp(src_y, dst_y, Y_SIZE) == 0, "Y plane matches");
    ASSERT(memcmp(src_u, dst_u, UV_SIZE) == 0, "U plane matches");
    ASSERT(memcmp(src_v, dst_v, UV_SIZE) == 0, "V plane matches");
    
    av1_copy_thread_destroy(ct);
    
    free(src_y); free(src_u); free(src_v);
    free(dst_y); free(dst_u); free(dst_v);
}

static void test_multiple_jobs(void) {
    printf("Test 2: Multiple jobs\n");
    
    Av1CopyThread *ct = av1_copy_thread_create(8);
    ASSERT(ct != NULL, "Copy thread created");
    
    uint8_t *src_y = aligned_alloc(32, 256);
    uint8_t *src_u = aligned_alloc(32, 64);
    uint8_t *src_v = aligned_alloc(32, 64);
    uint8_t *dst_y = aligned_alloc(32, 256);
    uint8_t *dst_u = aligned_alloc(32, 64);
    uint8_t *dst_v = aligned_alloc(32, 64);
    
    memset(src_y, 0xAA, 256);
    memset(src_u, 0xBB, 64);
    memset(src_v, 0xCC, 64);
    
    Av1CopyJob jobs[4];
    for (int i = 0; i < 4; i++) {
        memset(dst_y, 0, 256);
        jobs[i] = (Av1CopyJob){
            .frame_id = i,
            .dpb_slot = i,
            .src_planes = { src_y, src_u, src_v },
            .src_strides = { 16, 8, 8 },
            .dst_planes = { dst_y, dst_u, dst_v },
            .dst_strides = { 16, 8, 8 },
            .plane_widths = { 16, 8, 8 },
            .plane_heights = { 16, 8, 8 },
        };
        av1_copy_thread_enqueue(ct, &jobs[i]);
    }
    
    for (int i = 0; i < 4; i++) {
        av1_copy_thread_wait(ct, &jobs[i], 0);
    }
    
    ASSERT(av1_copy_thread_get_status(&jobs[3]) == AV1_COPY_COMPLETE, "All jobs completed");
    
    av1_copy_thread_destroy(ct);
    
    free(src_y); free(src_u); free(src_v);
    free(dst_y); free(dst_u); free(dst_v);
}

static void test_clean_shutdown(void) {
    printf("Test 3: Clean shutdown\n");
    
    Av1CopyThread *ct = av1_copy_thread_create(4);
    ASSERT(ct != NULL, "Copy thread created");
    
    usleep(10000);
    ASSERT(av1_copy_thread_destroy(ct) == 0, "Clean shutdown");
    
    printf("  PASS: Thread shut down cleanly\n");
    tests_passed++;
}

int main(void) {
    printf("=== AV1 Copy Thread Tests ===\n\n");
    
    test_single_copy();
    printf("\n");
    test_multiple_jobs();
    printf("\n");
    test_clean_shutdown();
    printf("\n");
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
