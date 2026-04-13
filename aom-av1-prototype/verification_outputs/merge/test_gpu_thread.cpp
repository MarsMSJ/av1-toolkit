#include "av1_gpu_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_JOBS 10
#define QUEUE_DEPTH 16

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while(0)

static void test_create_destroy(void) {
    printf("Test 1: Create and destroy\n");
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    usleep(10000);
    ASSERT(av1_gpu_thread_destroy(gt) == 0, "GPU thread destroyed");
}

static void test_single_job(void) {
    printf("Test 2: Single job\n");
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    Av1GpuJob job = { .frame_id = 42, .dpb_slot = 0, .needs_film_grain = 1, .dst_descriptor = NULL };
    
    ASSERT(av1_gpu_thread_enqueue(gt, &job) == 0, "Job enqueued");
    ASSERT(av1_gpu_thread_wait(gt, &job, 0) == 0, "Job completed");
    ASSERT(av1_gpu_thread_get_status(&job) == AV1_GPU_JOB_COMPLETE, "Status is COMPLETE");
    
    av1_gpu_thread_destroy(gt);
}

static void test_multiple_jobs(void) {
    printf("Test 3: Multiple jobs\n");
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    Av1GpuJob jobs[NUM_JOBS];
    for (int i = 0; i < NUM_JOBS; i++) {
        jobs[i].frame_id = static_cast<uint32_t>(i * 10);
        jobs[i].dpb_slot = i;
        jobs[i].needs_film_grain = (i % 2 == 0) ? 1 : 0;
        jobs[i].dst_descriptor = nullptr;
        jobs[i].status.store(0);
        av1_gpu_thread_enqueue(gt, &jobs[i]);
    }
    
    for (int i = 0; i < NUM_JOBS; i++) {
        av1_gpu_thread_wait(gt, &jobs[i], 0);
        ASSERT(av1_gpu_thread_get_status(&jobs[i]) == AV1_GPU_JOB_COMPLETE, "Job completed");
    }
    
    av1_gpu_thread_destroy(gt);
}

int main(void) {
    printf("=== AV1 GPU Thread Tests ===\n\n");
    
    test_create_destroy();
    printf("\n");
    test_single_job();
    printf("\n");
    test_multiple_jobs();
    printf("\n");
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
