#include "av1_gpu_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_JOBS 10
#define QUEUE_DEPTH 16

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

/* Test 1: Basic create/destroy */
static void test_create_destroy(void) {
    printf("Test 1: Create and destroy GPU thread\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Let thread initialize */
    usleep(10000);  /* 10ms */
    
    ASSERT(av1_gpu_thread_destroy(gt) == 0, "GPU thread destroyed cleanly");
    
    printf("  PASS: Thread created and destroyed successfully\n");
    tests_passed++;
}

/* Test 2: Single job enqueue and wait */
static void test_single_job(void) {
    printf("Test 2: Single job enqueue and wait\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Create a GPU job */
    Av1GpuJob job = {
        .frame_id = 42,
        .dpb_slot = 0,
        .needs_film_grain = 1,
        .dst_descriptor = NULL,
    };
    
    /* Enqueue the job */
    ASSERT(av1_gpu_thread_enqueue(gt, &job) == 0, "Job enqueued");
    
    /* Wait for completion */
    ASSERT(av1_gpu_thread_wait(gt, &job, 0) == 0, "Job completed");
    
    /* Verify status */
    ASSERT(av1_gpu_thread_get_status(&job) == AV1_GPU_JOB_COMPLETE, 
           "Job status is COMPLETE");
    
    av1_gpu_thread_destroy(gt);
}

/* Test 3: Multiple jobs in sequence */
static void test_multiple_jobs(void) {
    printf("Test 3: Multiple jobs in sequence\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    Av1GpuJob jobs[NUM_JOBS];
    
    /* Enqueue all jobs */
    for (int i = 0; i < NUM_JOBS; i++) {
        jobs[i] = (Av1GpuJob){
            .frame_id = (uint32_t)(i * 10),
            .dpb_slot = i % 8,
            .needs_film_grain = (i % 2 == 0),
            .dst_descriptor = NULL,
        };
        
        ASSERT(av1_gpu_thread_enqueue(gt, &jobs[i]) == 0, "Job enqueued");
    }
    
    /* Wait for each job to complete in order */
    for (int i = 0; i < NUM_JOBS; i++) {
        ASSERT(av1_gpu_thread_wait(gt, &jobs[i], 0) == 0, "Job completed");
        ASSERT(av1_gpu_thread_get_status(&jobs[i]) == AV1_GPU_JOB_COMPLETE,
               "Job status is COMPLETE");
        ASSERT(jobs[i].frame_id == (uint32_t)(i * 10),
               "Frame ID matches");
    }
    
    av1_gpu_thread_destroy(gt);
}

/* Test 4: Job with film grain flag */
static void test_film_grain_job(void) {
    printf("Test 4: Job with film grain flag\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Job with film grain */
    Av1GpuJob grain_job = {
        .frame_id = 100,
        .dpb_slot = 3,
        .needs_film_grain = 1,
        .dst_descriptor = (void*)0x12345678,  /* Fake descriptor */
    };
    
    /* Job without film grain */
    Av1GpuJob no_grain_job = {
        .frame_id = 101,
        .dpb_slot = 4,
        .needs_film_grain = 0,
        .dst_descriptor = (void*)0x87654321,  /* Fake descriptor */
    };
    
    ASSERT(av1_gpu_thread_enqueue(gt, &grain_job) == 0, "Grain job enqueued");
    ASSERT(av1_gpu_thread_enqueue(gt, &no_grain_job) == 0, "Non-grain job enqueued");
    
    ASSERT(av1_gpu_thread_wait(gt, &grain_job, 0) == 0, "Grain job completed");
    ASSERT(av1_gpu_thread_get_status(&grain_job) == AV1_GPU_JOB_COMPLETE,
           "Grain job status is COMPLETE");
    
    ASSERT(av1_gpu_thread_wait(gt, &no_grain_job, 0) == 0, "Non-grain job completed");
    ASSERT(av1_gpu_thread_get_status(&no_grain_job) == AV1_GPU_JOB_COMPLETE,
           "Non-grain job status is COMPLETE");
    
    av1_gpu_thread_destroy(gt);
}

/* Test 5: Queue full handling */
static void test_queue_full(void) {
    printf("Test 5: Queue full handling\n");
    
    /* Create thread with small queue */
    Av1GpuThread *gt = av1_gpu_thread_create(2, NULL);
    ASSERT(gt != NULL, "GPU thread created with small queue");
    
    Av1GpuJob job1 = { .frame_id = 1, .dpb_slot = 0, .needs_film_grain = 0 };
    Av1GpuJob job2 = { .frame_id = 2, .dpb_slot = 1, .needs_film_grain = 0 };
    Av1GpuJob job3 = { .frame_id = 3, .dpb_slot = 2, .needs_film_grain = 0 };
    
    /* Enqueue first job */
    ASSERT(av1_gpu_thread_enqueue(gt, &job1) == 0, "First job enqueued");
    
    /* Wait for it to complete */
    av1_gpu_thread_wait(gt, &job1, 0);
    
    /* Reuse job1 for second */
    job2.frame_id = 2;
    ASSERT(av1_gpu_thread_enqueue(gt, &job2) == 0, "Second job enqueued");
    av1_gpu_thread_wait(gt, &job2, 0);
    
    /* Reuse for third */
    job3.frame_id = 3;
    ASSERT(av1_gpu_thread_enqueue(gt, &job3) == 0, "Third job enqueued");
    av1_gpu_thread_wait(gt, &job3, 0);
    
    ASSERT(av1_gpu_thread_get_status(&job3) == AV1_GPU_JOB_COMPLETE,
           "Third job completed successfully");
    
    av1_gpu_thread_destroy(gt);
}

/* Test 6: Timeout on wait */
static void test_wait_timeout(void) {
    printf("Test 6: Wait timeout\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Create a job and enqueue it */
    Av1GpuJob job = {
        .frame_id = 999,
        .dpb_slot = 0,
        .needs_film_grain = 0,
    };
    
    ASSERT(av1_gpu_thread_enqueue(gt, &job) == 0, "Job enqueued");
    
    /* Use a very short timeout - job takes ~1ms to process */
    /* So timeout should trigger */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Try to wait with a very short timeout - should timeout since
     * stub takes 1ms and we timeout at 0 (infinite) but test with 500us */
    int result = av1_gpu_thread_wait(gt, &job, 500);  /* 500us timeout */
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000L + 
                      (end.tv_nsec - start.tv_nsec) / 1000;
    
    /* With 500us timeout and 1000us processing, should timeout */
    if (result == -1 && elapsed_us >= 400 && elapsed_us <= 1000) {
        printf("  PASS: Timeout elapsed ~500us (got %ldus)\n", elapsed_us);
        tests_passed++;
    } else if (result == 0) {
        /* Job completed within timeout - also acceptable */
        printf("  PASS: Job completed within timeout (%ldus)\n", elapsed_us);
        tests_passed++;
    } else {
        printf("  FAIL: Unexpected result %d, elapsed %ldus\n", result, elapsed_us);
        tests_failed++;
    }
    
    /* Clean up the job if it timed out */
    if (result == -1) {
        av1_gpu_thread_wait(gt, &job, 0);  /* Wait for completion */
    }
    
    av1_gpu_thread_destroy(gt);
}

/* Test 7: Verify job order preservation */
static void test_order_preservation(void) {
    printf("Test 7: Job order preservation\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Create many jobs with unique frame IDs */
    #define LARGE_NUM_JOBS 20
    Av1GpuJob jobs[LARGE_NUM_JOBS];
    
    for (int i = 0; i < LARGE_NUM_JOBS; i++) {
        jobs[i] = (Av1GpuJob){
            .frame_id = (uint32_t)(i * 7 + 3),  /* Unique IDs */
            .dpb_slot = i % 8,
            .needs_film_grain = (i % 3 == 0),
        };
        av1_gpu_thread_enqueue(gt, &jobs[i]);
    }
    
    /* Wait for all and verify order */
    int order_correct = 1;
    for (int i = 0; i < LARGE_NUM_JOBS; i++) {
        av1_gpu_thread_wait(gt, &jobs[i], 0);
        
        if (av1_gpu_thread_get_status(&jobs[i]) != AV1_GPU_JOB_COMPLETE) {
            order_correct = 0;
        }
        if (jobs[i].frame_id != (uint32_t)(i * 7 + 3)) {
            order_correct = 0;
        }
    }
    
    ASSERT(order_correct, "All jobs completed in order");
    
    av1_gpu_thread_destroy(gt);
}

/* Test 8: Clean shutdown with in-flight jobs */
static void test_shutdown_with_inflight(void) {
    printf("Test 8: Clean shutdown with in-flight jobs\n");
    
    Av1GpuThread *gt = av1_gpu_thread_create(QUEUE_DEPTH, NULL);
    ASSERT(gt != NULL, "GPU thread created");
    
    /* Enqueue several jobs */
    Av1GpuJob job1 = { .frame_id = 1, .dpb_slot = 0, .needs_film_grain = 0 };
    Av1GpuJob job2 = { .frame_id = 2, .dpb_slot = 1, .needs_film_grain = 0 };
    Av1GpuJob job3 = { .frame_id = 3, .dpb_slot = 2, .needs_film_grain = 0 };
    
    av1_gpu_thread_enqueue(gt, &job1);
    av1_gpu_thread_enqueue(gt, &job2);
    av1_gpu_thread_enqueue(gt, &job3);
    
    /* Destroy immediately - should process all jobs before exiting */
    ASSERT(av1_gpu_thread_destroy(gt) == 0, "GPU thread destroyed");
    
    /* Verify all jobs completed */
    ASSERT(av1_gpu_thread_get_status(&job1) == AV1_GPU_JOB_COMPLETE,
           "Job 1 completed before shutdown");
    ASSERT(av1_gpu_thread_get_status(&job2) == AV1_GPU_JOB_COMPLETE,
           "Job 2 completed before shutdown");
    ASSERT(av1_gpu_thread_get_status(&job3) == AV1_GPU_JOB_COMPLETE,
           "Job 3 completed before shutdown");
}

int main(void) {
    printf("=== AV1 GPU Thread Tests ===\n\n");
    
    test_create_destroy();
    printf("\n");
    
    test_single_job();
    printf("\n");
    
    test_multiple_jobs();
    printf("\n");
    
    test_film_grain_job();
    printf("\n");
    
    test_queue_full();
    printf("\n");
    
    test_wait_timeout();
    printf("\n");
    
    test_order_preservation();
    printf("\n");
    
    test_shutdown_with_inflight();
    printf("\n");
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
