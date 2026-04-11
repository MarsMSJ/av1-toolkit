#ifndef AV1_COPY_THREAD_H
#define AV1_COPY_THREAD_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* Copy job status values */
#define AV1_COPY_PENDING    0
#define AV1_COPY_IN_PROGRESS 1
#define AV1_COPY_COMPLETE   2

/* Maximum number of copy jobs in the queue */
#define AV1_COPY_QUEUE_DEPTH 8

typedef struct Av1CopyJob {
    uint32_t frame_id;
    int      dpb_slot;
    
    /* Source (internal DPB frame buffer) */
    const uint8_t *src_planes[3];
    int            src_strides[3];
    
    /* Destination (caller-provided buffer) */
    uint8_t       *dst_planes[3];
    int            dst_strides[3];
    
    /* Dimensions (width in bytes per plane, rows per plane) */
    int            plane_widths[3];
    int            plane_heights[3];
    
    /* Status - atomic for thread-safe access */
    _Atomic int    status;
} Av1CopyJob;

/* Opaque copy thread handle */
typedef struct Av1CopyThread Av1CopyThread;

/**
 * Create and start a copy thread.
 * @param queue_depth Number of jobs that can be queued (max)
 * @return Pointer to copy thread handle, or NULL on error
 */
Av1CopyThread* av1_copy_thread_create(int queue_depth);

/**
 * Enqueue a copy job to the copy thread.
 * @param ct   Copy thread handle
 * @param job  Copy job to enqueue
 * @return 0 on success, -1 on error (queue full or invalid args)
 */
int av1_copy_thread_enqueue(Av1CopyThread *ct, Av1CopyJob *job);

/**
 * Wait for a specific copy job to complete.
 * @param ct   Copy thread handle
 * @param job  Copy job to wait on
 * @param timeout_us Timeout in microseconds. 0 = infinite wait.
 * @return 0 on success (job completed), -1 on timeout or error
 */
int av1_copy_thread_wait(Av1CopyThread *ct, Av1CopyJob *job, uint32_t timeout_us);

/**
 * Get the current status of a copy job.
 * @param job  Copy job
 * @return Current status (AV1_COPY_PENDING, AV1_COPY_IN_PROGRESS, AV1_COPY_COMPLETE)
 */
int av1_copy_thread_get_status(Av1CopyJob *job);

/**
 * Destroy the copy thread (graceful shutdown).
 * Waits for any pending jobs to complete, then terminates the thread.
 * @param ct   Copy thread handle
 * @return 0 on success, -1 on error
 */
int av1_copy_thread_destroy(Av1CopyThread *ct);

/* GPU extension point - stub for future GPU offload */
#define AV1_COPY_FLAG_GPU 0x01

/**
 * Enqueue a copy job with options (GPU extension point).
 * @param ct     Copy thread handle
 * @param job    Copy job to enqueue
 * @param flags  Option flags (AV1_COPY_FLAG_GPU for GPU copy)
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_copy_thread_enqueue_ex(Av1CopyThread *ct, Av1CopyJob *job, int flags);

#endif /* AV1_COPY_THREAD_H */
