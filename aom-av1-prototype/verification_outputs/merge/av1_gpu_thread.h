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
 */
typedef struct Av1GpuJob {
    uint32_t frame_id;
    int      dpb_slot;
    int      needs_film_grain;
    void    *dst_descriptor;
    _Atomic int status;
} Av1GpuJob;

/**
 * GPU Thread handle.
 */
typedef struct Av1GpuThread {
    pthread_t         thread;
    pthread_mutex_t   mutex;
    pthread_cond_t    job_available;
    pthread_cond_t    job_complete;
    
    Av1GpuJob        *jobs;
    int               capacity;
    int               head;
    int               tail;
    int               count;
    
    int               exit_flag;
    void             *gpu_device;
} Av1GpuThread;

Av1GpuThread* av1_gpu_thread_create(int queue_depth, void *gpu_device);
int av1_gpu_thread_enqueue(Av1GpuThread *gt, Av1GpuJob *job);
int av1_gpu_thread_wait(Av1GpuThread *gt, Av1GpuJob *job, uint32_t timeout_us);
int av1_gpu_thread_get_status(Av1GpuJob *job);
int av1_gpu_thread_destroy(Av1GpuThread *gt);

#endif /* AV1_GPU_THREAD_H */
