#ifndef AV1_GPU_THREAD_H
#define AV1_GPU_THREAD_H

#include <cstdint>
#include <atomic>
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
    std::atomic<int> status;
} Av1GpuJob;

/**
 * GPU Thread handle.
 */
typedef struct Av1GpuThread {
    pthread_t         thread;
    pthread_mutex_t   mutex;
    pthread_cond_t    job_available;
    pthread_cond_t    job_complete;
    
    /* Internal queue entry - includes job pointer and metadata */
    struct Av1GpuQueueEntry {
        Av1GpuJob *job;
        int        enqueued;
    } *jobs;
    
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

/* GPU_IMPL: Extension points for GPU-accelerated decoder operations */
/* These stubs allow runtime dispatch to GPU implementations when available */

/**
 * GPU_IMPL: Upload YUV data to GPU memory.
 * @param src_planes    Source YUV plane pointers (system memory)
 * @param src_strides   Strides for source planes
 * @param dst_ptr       Destination GPU pointer (output)
 * @param widths        Plane widths in pixels
 * @param heights       Plane heights in pixels
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_upload(const uint8_t *src_planes[3], int src_strides[3],
//                                   void *dst_ptr, int widths[3], int heights[3]);

/**
 * GPU_IMPL: Inverse transform (IDCT, ADST, etc.) on GPU.
 * @param dpb_slot      DPB slot containing the frame
 * @param tx_type       Transform type
 * @param plane         Plane index (0=Y, 1=U, 2=V)
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_inverse_transform(int dpb_slot, int tx_type, int plane);

/**
 * GPU_IMPL: Intra prediction on GPU.
 * @param dpb_slot      DPB slot for current frame
 * @param mb_row        Macroblock row
 * @param mb_col        Macroblock column
 * @param mode          Intra prediction mode
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_intra_pred(int dpb_slot, int mb_row, int mb_col, int mode);

/**
 * GPU_IMPL: Inter prediction (motion compensation) on GPU.
 * @param dpb_slot      DPB slot for current frame
 * @param mb_row        Macroblock row
 * @param mb_col        Macroblock column
 * @param ref_slots     Reference frame DPB slots
 * @param mv_x          Motion vector X
 * @param mv_y          Motion vector Y
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_inter_pred(int dpb_slot, int mb_row, int mb_col,
//                                       int ref_slots[2], int mv_x, int mv_y);

/**
 * GPU_IMPL: Loop filters (deblock, CDEF, LR) on GPU.
 * @param dpb_slot      DPB slot containing the frame
 * @param filter_type   Filter type (deblock=0, CDEF=1, LR=2)
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_filter(int dpb_slot, int filter_type);

/**
 * GPU_IMPL: Film grain synthesis and final output on GPU.
 * @param dpb_slot      DPB slot containing the frame
 * @param output_ptr    Output GPU buffer (may be same as frame or separate)
 * @param apply_grain   Whether to apply film grain
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_gpu_impl_film_grain_output(int dpb_slot, void *output_ptr, int apply_grain);

#endif /* AV1_GPU_THREAD_H */
