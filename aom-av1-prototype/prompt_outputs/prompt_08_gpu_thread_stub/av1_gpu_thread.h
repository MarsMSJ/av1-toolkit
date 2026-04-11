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
