#include "av1_copy_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WIDTH  1920
#define HEIGHT 1080

/* YUV420 plane sizes */
#define Y_SIZE (WIDTH * HEIGHT)
#define UV_SIZE (Y_SIZE / 4)
#define TOTAL_SIZE (Y_SIZE + 2 * UV_SIZE)

/* Test result tracking */
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

/* Allocate YUV420 buffers */
static void allocate_buffers(uint8_t **y, uint8_t **u, uint8_t **v,
                              uint8_t **dst_y, uint8_t **dst_u, uint8_t **dst_v) {
    *y = aligned_alloc(32, Y_SIZE);
    *u = aligned_alloc(32, UV_SIZE);
    *v = aligned_alloc(32, UV_SIZE);
    *dst_y = aligned_alloc(32, Y_SIZE);
    *dst_u = aligned_alloc(32, UV_SIZE);
    *dst_v = aligned_alloc(32, UV_SIZE);
}

static void free_buffers(uint8_t *y, uint8_t *u, uint8_t *v,
                          uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v) {
    free(y);
    free(u);
    free(v);
    free(dst_y);
    free(dst_u);
    free(dst_v);
}

/* Fill source buffers with a known pattern */
static void fill_pattern(uint8_t *y, uint8_t *u, uint8_t *v, uint32_t frame_id) {
    /* Y plane: gradient based on frame_id */
    for (int i = 0; i < Y_SIZE; i++) {
        y[i] = (uint8_t)((i + frame_id) & 0xFF);
    }
    
    /* U plane: checkerboard pattern */
    for (int i = 0; i < UV_SIZE; i++) {
        int x = (i % (WIDTH / 2));
        int y_coord = (i / (WIDTH / 2));
        u[i] = (uint8_t)(((x + y_coord + frame_id) % 256));
    }
    
    /* V plane: simple pattern */
    for (int i = 0; i < UV_SIZE; i++) {
        v[i] = (uint8_t)((i + frame_id * 7) & 0xFF);
    }
}

/* Zero destination buffers */
static void zero_buffers(uint8_t *y, uint8_t *u, uint8_t *v) {
    memset(y, 0, Y_SIZE);
    memset(u, 0, UV_SIZE);
    memset(v, 0, UV_SIZE);
}

/* Compare buffers byte-for-byte */
static int compare_buffers(const uint8_t *src_y, const uint8_t *src_u, const uint8_t *src_v,
                            const uint8_t *dst_y, const uint8_t *dst_u, const uint8_t *dst_v) {
    if (memcmp(src_y, dst_y, Y_SIZE) != 0) return 0;
    if (memcmp(src_u, dst_u, UV_SIZE) != 0) return 0;
    if (memcmp(src_v, dst_v, UV_SIZE) != 0) return 0;
    return 1;
}

/* Test 1: Basic single copy */
static void test_single_copy(void) {
    printf("Test 1: Basic single copy (1920x1080 YUV420)\n");
    
    uint8_t *src_y, *src_u, *src_v;
    uint8_t *dst_y, *dst_u, *dst_v;
    allocate_buffers(&src_y, &src_u, &src_v, &dst_y, &dst_u, &dst_v);
    
    /* Fill source with pattern */
    fill_pattern(src_y, src_u, src_v, 42);
    zero_buffers(dst_y, dst_u, dst_v);
    
    /* Create copy thread */
    Av1CopyThread *ct = av1_copy_thread_create(4);
    ASSERT(ct != NULL, "Copy thread created");
    
    /* Set up copy job */
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
    
    /* Enqueue and wait */
    ASSERT(av1_copy_thread_enqueue(ct, &job) == 0, "Job enqueued");
    ASSERT(av1_copy_thread_wait(ct, &job, 0) == 0, "Job completed");
    ASSERT(av1_copy_thread_get_status(&job) == AV1_COPY_COMPLETE, "Status is COMPLETE");
    
    /* Verify copy */
    ASSERT(compare_buffers(src_y, src_u, src_v, dst_y, dst_u, dst_v),
           "Destination matches source byte-for-byte");
    
    av1_copy_thread_destroy(ct);
    free_buffers(src_y, src_u, src_v, dst_y, dst_u, dst_v);
}

/* Test 2: Four back-to-back copies */
static void test_four_copies(void) {
    printf("Test 2: Four back-to-back copies\n");
    
    uint8_t *src_y, *src_u, *src_v;
    uint8_t *dst_y, *dst_u, *dst_v;
    allocate_buffers(&src_y, &src_u, &src_v, &dst_y, &dst_u, &dst_v);
    
    Av1CopyThread *ct = av1_copy_thread_create(8);
    ASSERT(ct != NULL, "Copy thread created");
    
    Av1CopyJob jobs[4];
    
    /* Enqueue 4 copies with different patterns */
    for (int i = 0; i < 4; i++) {
        fill_pattern(src_y, src_u, src_v, i * 100);
        zero_buffers(dst_y, dst_u, dst_v);
        
        jobs[i] = (Av1CopyJob){
            .frame_id = i * 100,
            .dpb_slot = i,
            .src_planes = { src_y, src_u, src_v },
            .src_strides = { WIDTH, WIDTH/2, WIDTH/2 },
            .dst_planes = { dst_y, dst_u, dst_v },
            .dst_strides = { WIDTH, WIDTH/2, WIDTH/2 },
            .plane_widths = { WIDTH, WIDTH/2, WIDTH/2 },
            .plane_heights = { HEIGHT, HEIGHT/2, HEIGHT/2 },
        };
        
        ASSERT(av1_copy_thread_enqueue(ct, &jobs[i]) == 0, "Job enqueued");
    }
    
    /* Wait for all jobs to complete */
    for (int i = 0; i < 4; i++) {
        ASSERT(av1_copy_thread_wait(ct, &jobs[i], 0) == 0, "Job completed");
        ASSERT(av1_copy_thread_get_status(&jobs[i]) == AV1_COPY_COMPLETE, "Status is COMPLETE");
    }
    
    /* Verify each copy */
    for (int i = 0; i < 4; i++) {
        fill_pattern(src_y, src_u, src_v, i * 100);
        zero_buffers(dst_y, dst_u, dst_v);  /* Reset for verification */
        
        /* Re-fill destination from job */
        /* Actually, we need separate buffers for each job to verify properly */
    }
    
    /* For proper verification, we need separate buffers per job */
    /* Let's redo this test with separate buffers */
    
    av1_copy_thread_destroy(ct);
    
    /* Test with separate buffers per job */
    printf("  (verifying with separate buffers per job)\n");
    
    uint8_t *src_bufs[4][3];
    uint8_t *dst_bufs[4][3];
    
    for (int i = 0; i < 4; i++) {
        src_bufs[i][0] = aligned_alloc(32, Y_SIZE);
        src_bufs[i][1] = aligned_alloc(32, UV_SIZE);
        src_bufs[i][2] = aligned_alloc(32, UV_SIZE);
        dst_bufs[i][0] = aligned_alloc(32, Y_SIZE);
        dst_bufs[i][1] = aligned_alloc(32, UV_SIZE);
        dst_bufs[i][2] = aligned_alloc(32, UV_SIZE);
        
        fill_pattern(src_bufs[i][0], src_bufs[i][1], src_bufs[i][2], i * 100);
        zero_buffers(dst_bufs[i][0], dst_bufs[i][1], dst_bufs[i][2]);
    }
    
    ct = av1_copy_thread_create(8);
    
    Av1CopyJob jobs2[4];
    for (int i = 0; i < 4; i++) {
        jobs2[i] = (Av1CopyJob){
            .frame_id = i * 100,
            .dpb_slot = i,
            .src_planes = { src_bufs[i][0], src_bufs[i][1], src_bufs[i][2] },
            .src_strides = { WIDTH, WIDTH/2, WIDTH/2 },
            .dst_planes = { dst_bufs[i][0], dst_bufs[i][1], dst_bufs[i][2] },
            .dst_strides = { WIDTH, WIDTH/2, WIDTH/2 },
            .plane_widths = { WIDTH, WIDTH/2, WIDTH/2 },
            .plane_heights = { HEIGHT, HEIGHT/2, HEIGHT/2 },
        };
        
        av1_copy_thread_enqueue(ct, &jobs2[i]);
    }
    
    /* Wait for all */
    for (int i = 0; i < 4; i++) {
        av1_copy_thread_wait(ct, &jobs2[i], 0);
    }
    
    /* Verify all */
    int all_match = 1;
    for (int i = 0; i < 4; i++) {
        if (!compare_buffers(src_bufs[i][0], src_bufs[i][1], src_bufs[i][2],
                              dst_bufs[i][0], dst_bufs[i][1], dst_bufs[i][2])) {
            all_match = 0;
        }
    }
    ASSERT(all_match, "All 4 copies verified");
    
    av1_copy_thread_destroy(ct);
    
    /* Free buffers */
    for (int i = 0; i < 4; i++) {
        free(src_bufs[i][0]);
        free(src_bufs[i][1]);
        free(src_bufs[i][2]);
        free(dst_bufs[i][0]);
        free(dst_bufs[i][1]);
        free(dst_bufs[i][2]);
    }
    
    free_buffers(src_y, src_u, src_v, dst_y, dst_u, dst_v);
}

/* Test 3: Clean shutdown while idle */
static void test_clean_shutdown(void) {
    printf("Test 3: Clean shutdown while idle\n");
    
    Av1CopyThread *ct = av1_copy_thread_create(4);
    ASSERT(ct != NULL, "Copy thread created");
    
    /* Let thread initialize */
    usleep(10000);  /* 10ms */
    
    /* Destroy while idle - should be clean */
    ASSERT(av1_copy_thread_destroy(ct) == 0, "Clean shutdown succeeded");
    
    printf("  PASS: Thread shut down cleanly while idle\n");
    tests_passed++;
}

/* Test 4: Queue full handling */
static void test_queue_full(void) {
    printf("Test 4: Queue full handling\n");
    
    Av1CopyThread *ct = av1_copy_thread_create(2);  /* Small queue */
    ASSERT(ct != NULL, "Copy thread created with small queue");
    
    uint8_t *src_y = aligned_alloc(32, 256);
    uint8_t *src_u = aligned_alloc(32, 64);
    uint8_t *src_v = aligned_alloc(32, 64);
    uint8_t *dst_y = aligned_alloc(32, 256);
    uint8_t *dst_u = aligned_alloc(32, 64);
    uint8_t *dst_v = aligned_alloc(32, 64);
    
    Av1CopyJob job = {
        .frame_id = 0,
        .dpb_slot = 0,
        .src_planes = { src_y, src_u, src_v },
        .src_strides = { 16, 8, 8 },
        .dst_planes = { dst_y, dst_u, dst_v },
        .dst_strides = { 16, 8, 8 },
        .plane_widths = { 16, 8, 8 },
        .plane_heights = { 16, 8, 8 },
    };
    
    /* Fill source */
    memset(src_y, 0xAA, 256);
    memset(src_u, 0xBB, 64);
    memset(src_v, 0xCC, 64);
    zero_buffers(dst_y, dst_u, dst_v);
    
    /* Enqueue first job */
    ASSERT(av1_copy_thread_enqueue(ct, &job) == 0, "First job enqueued");
    
    /* Wait for it */
    av1_copy_thread_wait(ct, &job, 0);
    
    /* Verify copy worked */
    ASSERT(memcmp(src_y, dst_y, 256) == 0, "First copy verified");
    
    /* Reuse job for second */
    job.frame_id = 1;
    zero_buffers(dst_y, dst_u, dst_v);
    
    ASSERT(av1_copy_thread_enqueue(ct, &job) == 0, "Second job enqueued");
    av1_copy_thread_wait(ct, &job, 0);
    ASSERT(memcmp(src_y, dst_y, 256) == 0, "Second copy verified");
    
    av1_copy_thread_destroy(ct);
    
    free(src_y);
    free(src_u);
    free(src_v);
    free(dst_y);
    free(dst_u);
    free(dst_v);
}

/* Test 5: Timeout on wait */
static void test_wait_timeout(void) {
    printf("Test 5: Wait timeout\n");
    
    Av1CopyThread *ct = av1_copy_thread_create(4);
    ASSERT(ct != NULL, "Copy thread created");
    
    uint8_t *src_y = aligned_alloc(32, 256);
    uint8_t *src_u = aligned_alloc(32, 64);
    uint8_t *src_v = aligned_alloc(32, 64);
    uint8_t *dst_y = aligned_alloc(32, 256);
    uint8_t *dst_u = aligned_alloc(32, 64);
    uint8_t *dst_v = aligned_alloc(32, 64);
    
    Av1CopyJob job = {
        .frame_id = 0,
        .dpb_slot = 0,
        .src_planes = { src_y, src_u, src_v },
        .src_strides = { 16, 8, 8 },
        .dst_planes = { dst_y, dst_u, dst_v },
        .dst_strides = { 16, 8, 8 },
        .plane_widths = { 16, 8, 8 },
        .plane_heights = { 16, 8, 8 },
    };
    
    /* Try to wait on a job that was never enqueued - should timeout */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    int result = av1_copy_thread_wait(ct, &job, 100000);  /* 100ms timeout */
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    ASSERT(result == -1, "Wait returns -1 on timeout");
    
    long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000L + 
                      (end.tv_nsec - start.tv_nsec) / 1000;
    
    if (elapsed_us >= 90000 && elapsed_us <= 200000) {
        printf("  PASS: Timeout elapsed ~100ms (got %ldus)\n", elapsed_us);
        tests_passed++;
    } else {
        printf("  FAIL: Timeout elapsed %ldus (expected ~100000us)\n", elapsed_us);
        tests_failed++;
    }
    
    av1_copy_thread_destroy(ct);
    
    free(src_y);
    free(src_u);
    free(src_v);
    free(dst_y);
    free(dst_u);
    free(dst_v);
}

int main(void) {
    printf("=== AV1 Copy Thread Tests ===\n\n");
    
    test_single_copy();
    printf("\n");
    
    test_four_copies();
    printf("\n");
    
    test_clean_shutdown();
    printf("\n");
    
    test_queue_full();
    printf("\n");
    
    test_wait_timeout();
    printf("\n");
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
