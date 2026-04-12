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
 * Internal queue entry structure (not exposed in header).
 * Contains job pointer and enqueued flag for queue management.
 */
typedef struct Av1GpuQueueEntry {
    Av1GpuJob *job;
    int        enqueued;
} Av1GpuQueueEntry;

/**
 * GPU Thread handle.
 */
typedef struct Av1GpuThread {
    pthread_t         thread;
    pthread_mutex_t   mutex;
    pthread_cond_t    job_available;
    pthread_cond_t    job_complete;
    
    Av1GpuQueueEntry *jobs;  /* Fixed: was Av1GpuJob*, should be Av1GpuQueueEntry* */
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

/* GPU extension points - stubs for future GPU offload */
#define AV1_GPU_FLAG_UPLOAD         0x01
#define AV1_GPU_FLAG_INV_TX         0x02
#define AV1_GPU_FLAG_INTRA_PRED     0x04
#define AV1_GPU_FLAG_INTER_PRED     0x08
#define AV1_GPU_FLAG_FILTERS        0x10
#define AV1_GPU_FLAG_FILM_GRAIN     0x20

/**
 * Enqueue a GPU job with specific processing flags.
 * @param gt     GPU thread handle
 * @param job    GPU job to enqueue
 * @param flags  Processing flags (AV1_GPU_FLAG_*)
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_thread_enqueue_ex(Av1GpuThread *gt, Av1GpuJob *job, int flags);

/**
 * Upload YUV data to GPU memory.
 * @param src_planes   Source YUV plane pointers
 * @param src_strides  Source plane strides
 * @param widths       Plane widths
 * @param heights      Plane heights
 * @param gpu_buffer   Output GPU buffer handle
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_upload(const uint8_t *src_planes[3], int src_strides[3],
//                             int widths[3], int heights[3], void *gpu_buffer);

/**
 * Perform inverse transform on GPU.
 * @param dpb_slot    DPB slot containing coefficients
 * @param gpu_buffer  GPU buffer with transform coefficients
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_inv_transform(int dpb_slot, void *gpu_buffer);

/**
 * Perform intra prediction on GPU.
 * @param dpb_slot    DPB slot for prediction
 * @param block_size  Block size for prediction
 * @param mode        Intra prediction mode
 * @param gpu_output  Output GPU buffer
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_intra_pred(int dpb_slot, int block_size, int mode, void *gpu_output);

/**
 * Perform inter prediction on GPU.
 * @param dpb_slot    Current DPB slot
 * @param ref_slot    Reference frame DPB slot
 * @param mv          Motion vector
 * @param gpu_output  Output GPU buffer
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_inter_pred(int dpb_slot, int ref_slot, const int mv[2], void *gpu_output);

/**
 * Apply loop filters on GPU.
 * @param dpb_slot    DPB slot to filter
 * @param filter_type Filter type (deblock, CDEF, LR)
 * @param gpu_buffer  GPU buffer for filter parameters
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_apply_filters(int dpb_slot, int filter_type, void *gpu_buffer);

/**
 * Apply film grain and output on GPU.
 * @param dpb_slot    DPB slot
 * @param grain_params Film grain parameters
 * @param gpu_output  Output GPU buffer
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_film_grain_output(int dpb_slot, const void *grain_params, void *gpu_output);

#endif /* AV1_GPU_THREAD_H */
