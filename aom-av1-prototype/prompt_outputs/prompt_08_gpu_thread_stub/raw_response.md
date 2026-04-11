

### av1_gpu_thread.h
```c
#ifndef AV1_GPU_THREAD_H
#define AV1_GPU_THREAD_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* GPU job status values */
#define AV1_GPU_JOB_PENDING    0
#define AV1_GPU_JOB_PROCESSING 1
#define AV1_GPU_JOB_COMPLETE   2

/* Maximum number of GPU jobs in the queue */
#define AV1_GPU_QUEUE_DEPTH 8

/**
 * GPU Job structure.
 * Represents a frame to be processed on the GPU.
 * In a real implementation, this would contain GPU memory handles,
 * shader parameters, and command buffer references.
 */
typedef struct Av1GpuJob {
    uint32_t frame_id;           /* Frame identifier */
    int      dpb_slot;           /* DPB slot containing reference frames */
    int      needs_film_grain;   /* 1 if film grain synthesis required */
    
    /**
     * Opaque destination descriptor.
     * In a real implementation, this would be a GPU texture/buffer handle
     * (e.g., Vulkan VkImage, D3D12 texture, CUDA array) that specifies
     * where the final output should be written.
     * 
     * For film grain path: the GPU compute shader writes directly to
     * this destination buffer, performing format conversion (YUV420 -> NV12,
     * 10-bit packed, etc.) as part of the shader.
     */
    void    *dst_descriptor;
    
    /* Status - atomic for thread-safe access */
    _Atomic int status;
} Av1GpuJob;

/**
 * GPU Thread handle.
 * Manages a worker thread that processes GPU jobs.
 * 
 * In a real implementation, this would hold:
 * - GPU device handle (Vulkan device, D3D12 device, CUDA context)
 * - Command pool for allocating command buffers
 * - Descriptor set pools
 * - Fence pool for synchronization
 * - Memory pools for uploading symbol data and frame data
 */
typedef struct Av1GpuThread {
    pthread_t         thread;      /* Worker thread */
    pthread_mutex_t   mutex;       /* Queue synchronization */
    pthread_cond_t    job_available;   /* Signaled when job is enqueued */
    pthread_cond_t    job_complete;    /* Signaled when job completes */
    
    Av1GpuJob        *jobs;        /* Circular buffer for jobs */
    int               capacity;    /* Max queue entries */
    int               head;        /* Next to dequeue */
    int               tail;        /* Next to enqueue */
    int               count;       /* Current jobs in queue */
    
    int               exit_flag;   /* 1 when thread should exit */
    
    /**
     * Opaque GPU device handle.
     * In a real implementation:
     * - Vulkan: VkDevice
     * - D3D12: ID3D12Device*
     * - CUDA: CUdevice
     * - OpenCL: cl_device_id
     */
    void             *gpu_device;
} Av1GpuThread;

/**
 * Create and start a GPU thread.
 * @param queue_depth Number of jobs that can be queued (max)
 * @param gpu_device  Opaque GPU device handle (can be NULL for stub)
 * @return Pointer to GPU thread handle, or NULL on error
 */
Av1GpuThread* av1_gpu_thread_create(int queue_depth, void *gpu_device);

/**
 * Enqueue a GPU job to the GPU thread.
 * @param gt   GPU thread handle
 * @param job  GPU job to enqueue
 * @return 0 on success, -1 on error (queue full or invalid args)
 */
int av1_gpu_thread_enqueue(Av1GpuThread *gt, Av1GpuJob *job);

/**
 * Wait for a specific GPU job to complete.
 * @param gt         GPU thread handle
 * @param job        GPU job to wait on
 * @param timeout_us Timeout in microseconds. 0 = infinite wait.
 * @return 0 on success (job completed), -1 on timeout or error
 */
int av1_gpu_thread_wait(Av1GpuThread *gt, Av1GpuJob *job, uint32_t timeout_us);

/**
 * Get the current status of a GPU job.
 * @param job  GPU job
 * @return Current status (AV1_GPU_JOB_PENDING, AV1_GPU_JOB_PROCESSING, AV1_GPU_JOB_COMPLETE)
 */
int av1_gpu_thread_get_status(Av1GpuJob *job);

/**
 * Destroy the GPU thread (graceful shutdown).
 * Waits for any pending jobs to complete, then terminates the thread.
 * @param gt   GPU thread handle
 * @return 0 on success, -1 on error
 */
int av1_gpu_thread_destroy(Av1GpuThread *gt);

/* ========================================================================
 * GPU Implementation Notes (for reference, not implemented):
 * ========================================================================
 *
 * A real GPU implementation would perform the following in the worker:
 *
 * 1. UPLOAD PHASE:
 *    - Upload quantized coefficients from CPU to GPU memory
 *    - Upload segment map, skip map, cdef strength, lr params
 *    - Upload reference frame pointers (DPB slots)
 *
 * 2. RECONSTRUCTION PHASE:
 *    - Dispatch inverse transform compute shader (DCT, ADST, etc.)
 *    - Dispatch intra prediction compute shader (all 56 modes)
 *    - Dispatch inter prediction compute shader (motion compensation)
 *    - Handle wedge masks, compound convolution, warped motion
 *
 * 3. FILTERING PHASE:
 *    - Dispatch loop filter (deblock) compute shader
 *    - Dispatch CDEF (Constrained Directional Enhancement Filter)
 *    - Dispatch LR (Loop Restoration) with Wiener or SGR
 *
 * 4. FILM GRAIN + OUTPUT PHASE:
 *    - If needs_film_grain is set:
 *      * Read un-grained frame from DPB (GPU memory)
 *      * Synthesize film grain per AV1 spec (Gaussian model)
 *      * Apply grain to luma and chroma with proper clipping
 *      * Convert pixel format (YUV420 -> NV12, P010, etc.)
 *      * Write directly to dst_descriptor buffer
 *    - Else:
 *      * Copy frame to destination with format conversion
 *
 * 5. SYNCHRONIZATION:
 *    - Submit command buffer to GPU queue
 *    - Create fence and wait for completion
 *    - Signal job_complete condition variable
 *
 * The Copy Thread (av1_copy_thread) is NOT used in GPU mode because
 * the film grain shader performs the copy + conversion + grain synthesis
 * as a single GPU operation.
 */

#endif /* AV1_GPU_THREAD_H */
```

### av1_gpu_thread.c
```c
#include "av1_gpu_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define NSEC_PER_USEC 1000ULL
#define USEC_PER_SEC  1000000ULL

/* Stub processing delay in microseconds */
#define STUB_PROCESSING_DELAY_US 1000

/* Internal queue entry */
typedef struct Av1GpuQueueEntry {
    Av1GpuJob *job;
    int        enqueued;  /* 1 if entry contains a valid job */
} Av1GpuQueueEntry;

/* Forward declarations */
static void* gpu_thread_main(void *arg);
static void gpu_job_execute(Av1GpuJob *job);

Av1GpuThread* av1_gpu_thread_create(int queue_depth, void *gpu_device) {
    if (queue_depth <= 0) {
        queue_depth = AV1_GPU_QUEUE_DEPTH;
    }
    
    Av1GpuThread *gt = calloc(1, sizeof(Av1GpuThread));
    if (!gt) {
        return NULL;
    }
    
    gt->jobs = calloc(queue_depth, sizeof(Av1GpuQueueEntry));
    if (!gt->jobs) {
        free(gt);
        return NULL;
    }
    
    gt->capacity = queue_depth;
    gt->head = 0;
    gt->tail = 0;
    gt->count = 0;
    gt->exit_flag = 0;
    gt->gpu_device = gpu_device;
    
    /* Initialize synchronization primitives */
    if (pthread_mutex_init(&gt->mutex, NULL) != 0) {
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_available, NULL) != 0) {
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_complete, NULL) != 0) {
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    /* Start the GPU thread */
    if (pthread_create(&gt->thread, NULL, gpu_thread_main, gt) != 0) {
        pthread_cond_destroy(&gt->job_complete);
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        free(gt->jobs);
        free(gt);
        return NULL;
    }
    
    return gt;
}

int av1_gpu_thread_enqueue(Av1GpuThread *gt, Av1GpuJob *job) {
    if (!gt || !job) {
        return -1;
    }
    
    /* Initialize job status */
    atomic_store(&job->status, AV1_GPU_JOB_PENDING);
    
    pthread_mutex_lock(&gt->mutex);
    
    /* Check if queue is full */
    if (gt->count >= gt->capacity) {
        pthread_mutex_unlock(&gt->mutex);
        return -1;
    }
    
    /* Enqueue the job */
    gt->jobs[gt->tail].job = job;
    gt->jobs[gt->tail].enqueued = 1;
    gt->tail = (gt->tail + 1) % gt->capacity;
    gt->count++;
    
    /* Signal the GPU thread that a job is available */
    pthread_cond_signal(&gt->job_available);
    
    pthread_mutex_unlock(&gt->mutex);
    
    return 0;
}

int av1_gpu_thread_wait(Av1GpuThread *gt, Av1GpuJob *job, uint32_t timeout_us) {
    if (!gt || !job) {
        return -1;
    }
    
    pthread_mutex_lock(&gt->mutex);
    
    /* Check current status without waiting */
    int current_status = atomic_load(&job->status);
    if (current_status == AV1_GPU_JOB_COMPLETE) {
        pthread_mutex_unlock(&gt->mutex);
        return 0;
    }
    
    if (current_status == AV1_GPU_JOB_PROCESSING) {
        /* Job is being processed, wait for completion */
        if (timeout_us == 0) {
            /* Infinite wait */
            while (atomic_load(&job->status) == AV1_GPU_JOB_PROCESSING) {
                pthread_cond_wait(&gt->job_complete, &gt->mutex);
            }
        } else {
            /* Timed wait */
            struct timespec ts;
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                pthread_mutex_unlock(&gt->mutex);
                return -1;
            }
            
            uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * NSEC_PER_USEC;
            ts.tv_sec += (int)(nsec / 1000000000ULL);
            ts.tv_nsec = (long)(nsec % 1000000000ULL);
            
            while (atomic_load(&job->status) == AV1_GPU_JOB_PROCESSING) {
                int ret = pthread_cond_timedwait(&gt->job_complete, &gt->mutex, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&gt->mutex);
                    return -1;
                }
                if (ret != 0) {
                    pthread_mutex_unlock(&gt->mutex);
                    return -1;
                }
            }
        }
    }
    
    /* Check final status */
    int final_status = atomic_load(&job->status);
    pthread_mutex_unlock(&gt->mutex);
    
    return (final_status == AV1_GPU_JOB_COMPLETE) ? 0 : -1;
}

int av1_gpu_thread_get_status(Av1GpuJob *job) {
    if (!job) {
        return -1;
    }
    return atomic_load(&job->status);
}

int av1_gpu_thread_destroy(Av1GpuThread *gt) {
    if (!gt) {
        return -1;
    }
    
    pthread_mutex_lock(&gt->mutex);
    gt->exit_flag = 1;
    pthread_cond_broadcast(&gt->job_available);  /* Wake thread to exit */
    pthread_mutex_unlock(&gt->mutex);
    
    /* Wait for thread to terminate */
    pthread_join(gt->thread, NULL);
    
    /* Clean up synchronization primitives */
    pthread_cond_destroy(&gt->job_complete);
    pthread_cond_destroy(&gt->job_available);
    pthread_mutex_destroy(&gt->mutex);
    
    /* Free resources */
    free(gt->jobs);
    free(gt);
    
    return 0;
}

/* ========================================================================
 * GPU Thread Main Function
 * ========================================================================
 * This is the STUB implementation. A real GPU implementation would:
 * 
 * 1. Upload symbol data (quantized coefficients, segment maps, etc.)
 * 2. Build inverse transform compute dispatch
 * 3. Build intra/inter prediction dispatch
 * 4. Build loop filter + CDEF + LR dispatch
 * 5. Build film grain + format convert + output dispatch
 * 6. Submit command buffer and wait on fence
 * 
 * The stub simply sleeps for a short time to simulate GPU processing.
 */
static void* gpu_thread_main(void *arg) {
    Av1GpuThread *gt = (Av1GpuThread*)arg;
    
    pthread_mutex_lock(&gt->mutex);
    
    while (!gt->exit_flag) {
        /* Wait for a job to become available */
        while (gt->count == 0 && !gt->exit_flag) {
            pthread_cond_wait(&gt->job_available, &gt->mutex);
        }
        
        /* Check if we should exit */
        if (gt->exit_flag) {
            break;
        }
        
        /* Dequeue the job */
        Av1GpuQueueEntry *entry = &gt->jobs[gt->head];
        Av1GpuJob *job = entry->job;
        
        gt->head = (gt->head + 1) % gt->capacity;
        gt->count--;
        
        /* Mark entry as empty */
        entry->enqueued = 0;
        
        /* Set status to PROCESSING and release lock during processing */
        atomic_store(&job->status, AV1_GPU_JOB_PROCESSING);
        
        pthread_mutex_unlock(&gt->mutex);
        
        /* =================================================================
         * GPU_IMPL: Upload symbol data here
         * - Copy quantized coefficients from CPU to GPU memory
         * - Upload segment map, skip map, cdef strength, lr params
         * - Upload reference frame pointers (DPB slots)
         */
        
        /* =================================================================
         * GPU_IMPL: Build inverse transform compute dispatch here
         * - DCT inverse for luma and chroma blocks
         * - ADST inverse for intra blocks
         * - Identity, flipadst variants
         */
        
        /* =================================================================
         * GPU_IMPL: Build intra prediction dispatch here
         * - All 56 intra prediction modes
         * - Angular, DC, Paeth, smooth, smooth_h, smooth_v
         * - Palette mode
         */
        
        /* =================================================================
         * GPU_IMPL: Build inter prediction dispatch here
         * - Motion compensation from reference frames
         * - Compound prediction (wedge, distance-weighted)
         * - Warped motion models
         */
        
        /* =================================================================
         * GPU_IMPL: Build loop filter + CDEF + LR dispatch here
         * - Deblock filter (horizontal and vertical passes)
         * - CDEF (Constrained Directional Enhancement Filter)
         * - Loop Restoration (Wiener or Self-Guided)
         */
        
        /* =================================================================
         * GPU_IMPL: Build film grain + format convert + output dispatch
         * - If needs_film_grain:
         *   * Read un-grained frame from DPB (GPU memory)
         *   * Synthesize film grain per AV1 spec (Gaussian model)
         *   * Apply grain to luma and chroma with proper clipping
         *   * Convert pixel format (YUV420 -> NV12, P010, etc.)
         *   * Write directly to dst_descriptor buffer
         * - Else:
         *   * Simple format conversion and copy to destination
         * 
         * Note: Copy Thread is NOT used in GPU mode because the
         * film grain shader performs copy + conversion + grain
         * synthesis as a single GPU operation.
         */
        
        /* =================================================================
         * GPU_IMPL: Submit command buffer and wait on fence here
         * - Submit all recorded command buffers to GPU queue
         * - Create fence and submit to GPU
         * - Wait for fence to signal completion
         */
        
        /* STUB: Simulate GPU processing delay */
        usleep(STUB_PROCESSING_DELAY_US);
        
        /* Mark job as complete */
        atomic_store(&job->status, AV1_GPU_JOB_COMPLETE);
        
        /* Signal any waiters that the job is done */
        pthread_mutex_lock(&gt->mutex);
        pthread_cond_broadcast(&gt->job_complete);
    }
    
    pthread_mutex_unlock(&gt->mutex);
    
    return NULL;
}

/* Stub execution - in real impl this would be GPU kernels */
static void gpu_job_execute(Av1GpuJob *job) {
    if (!job) {
        return;
    }
    
    /* STUB: The actual GPU work would happen here in a real implementation:
     *
     * 1. Upload phase:
     *    - cuMemcpyHtoD(coeffs_gpu, coeffs_cpu, size)
     *    - cuMemcpyHtoD(segmap_gpu, segmap_cpu, size)
     *
     * 2. Reconstruction:
     *    - cuLaunchKernel(inverse_transform, ...)
     *    - cuLaunchKernel(intra_pred, ...)
     *    - cuLaunchKernel(inter_pred, ...)
     *
     * 3. Filtering:
     *    - cuLaunchKernel(deblock, ...)
     *    - cuLaunchKernel(cdef, ...)
     *    - cuLaunchKernel(loop_restoration, ...)
     *
     * 4. Film grain + output:
     *    - if (job->needs_film_grain) {
     *        cuLaunchKernel(film_grain_synth, ...)
     *        cuLaunchKernel(format_convert, ...)
     *      }
     *    - cuMemcpyDtoD(dst_gpu, frame_gpu, size)
     *
     * 5. Synchronization:
     *    - cuEventSynchronize(completion_event)
     *
     * For now, we just simulate the delay in gpu_thread_main()
     */
    
    (void)job;  /* Suppress unused warning */
}
```

### test_gpu_thread.c
```c
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
    
    /* Job that is never enqueued */
    Av1GpuJob orphan_job = {
        .frame_id = 999,
        .dpb_slot = 0,
        .needs_film_grain = 0,
    };
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Try to wait on a job that was never enqueued - should timeout */
    int result = av1_gpu_thread_wait(gt, &orphan_job, 100000);  /* 100ms */
    
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
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
```