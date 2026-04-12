#include "av1_gpu_thread.h"
#include "av1_mem_override.h"  /* Added for custom memory allocator */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define NSEC_PER_USEC 1000ULL
#define STUB_PROCESSING_DELAY_US 1000

static void* gpu_thread_main(void *arg);

Av1GpuThread* av1_gpu_thread_create(int queue_depth, void *gpu_device) {
    if (queue_depth <= 0) {
        queue_depth = AV1_GPU_QUEUE_DEPTH;
    }
    
    /* Use custom allocator if override is active */
    Av1GpuThread *gt = av1_mem_malloc ? av1_mem_malloc(sizeof(Av1GpuThread), AV1_MEM_FLAG_ZERO) 
                                      : calloc(1, sizeof(Av1GpuThread));
    if (!gt) {
        return NULL;
    }
    
    /* Use custom allocator for jobs array */
    size_t jobs_size = queue_depth * sizeof(Av1GpuQueueEntry);
    gt->jobs = av1_mem_malloc ? av1_mem_malloc(jobs_size, AV1_MEM_FLAG_ZERO)
                              : calloc(queue_depth, sizeof(Av1GpuQueueEntry));
    if (!gt->jobs) {
        if (av1_mem_free) av1_mem_free(gt);
        else free(gt);
        return NULL;
    }
    
    gt->capacity = queue_depth;
    gt->head = 0;
    gt->tail = 0;
    gt->count = 0;
    gt->exit_flag = 0;
    gt->gpu_device = gpu_device;
    
    if (pthread_mutex_init(&gt->mutex, NULL) != 0) {
        if (av1_mem_free) av1_mem_free(gt->jobs);
        else free(gt->jobs);
        if (av1_mem_free) av1_mem_free(gt);
        else free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_available, NULL) != 0) {
        pthread_mutex_destroy(&gt->mutex);
        if (av1_mem_free) av1_mem_free(gt->jobs);
        else free(gt->jobs);
        if (av1_mem_free) av1_mem_free(gt);
        else free(gt);
        return NULL;
    }
    
    if (pthread_cond_init(&gt->job_complete, NULL) != 0) {
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        if (av1_mem_free) av1_mem_free(gt->jobs);
        else free(gt->jobs);
        if (av1_mem_free) av1_mem_free(gt);
        else free(gt);
        return NULL;
    }
    
    if (pthread_create(&gt->thread, NULL, gpu_thread_main, gt) != 0) {
        pthread_cond_destroy(&gt->job_complete);
        pthread_cond_destroy(&gt->job_available);
        pthread_mutex_destroy(&gt->mutex);
        if (av1_mem_free) av1_mem_free(gt->jobs);
        else free(gt->jobs);
        if (av1_mem_free) av1_mem_free(gt);
        else free(gt);
        return NULL;
    }
    
    return gt;
}

int av1_gpu_thread_enqueue(Av1GpuThread *gt, Av1GpuJob *job) {
    if (!gt || !job) {
        return -1;
    }
    
    atomic_store(&job->status, AV1_GPU_JOB_PENDING);
    
    pthread_mutex_lock(&gt->mutex);
    
    if (gt->count >= gt->capacity) {
        pthread_mutex_unlock(&gt->mutex);
        return -1;
    }
    
    gt->jobs[gt->tail].job = job;
    gt->jobs[gt->tail].enqueued = 1;
    gt->tail = (gt->tail + 1) % gt->capacity;
    gt->count++;
    
    pthread_cond_signal(&gt->job_available);
    
    pthread_mutex_unlock(&gt->mutex);
    
    return 0;
}

int av1_gpu_thread_wait(Av1GpuThread *gt, Av1GpuJob *job, uint32_t timeout_us) {
    if (!gt || !job) {
        return -1;
    }
    
    pthread_mutex_lock(&gt->mutex);
    
    /* Fixed: Wait for both PENDING and PROCESSING states */
    int current_status = atomic_load(&job->status);
    
    /* Wait until job is complete or being processed */
    while (current_status != AV1_GPU_JOB_COMPLETE) {
        if (current_status == AV1_GPU_JOB_PENDING) {
            /* Job not yet picked up - wait for it to be processed */
            if (timeout_us == 0) {
                /* Wait indefinitely for job to be picked up */
                pthread_cond_wait(&gt->job_available, &gt->mutex);
                current_status = atomic_load(&job->status);
                if (current_status == AV1_GPU_JOB_COMPLETE) {
                    break;
                }
                /* Now it should be PROCESSING, fall through to wait for completion */
                if (current_status != AV1_GPU_JOB_PROCESSING) {
                    /* Still pending, continue waiting */
                    continue;
                }
            } else {
                /* Timeout: wait for job to be picked up or completed */
                struct timespec ts;
                if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                    pthread_mutex_unlock(&gt->mutex);
                    return -1;
                }
                
                uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * NSEC_PER_USEC;
                ts.tv_sec += (int)(nsec / 1000000000ULL);
                ts.tv_nsec = (long)(nsec % 1000000000ULL);
                
                int ret = pthread_cond_timedwait(&gt->job_available, &gt->mutex, &ts);
                if (ret == ETIMEDOUT) {
                    pthread_mutex_unlock(&gt->mutex);
                    return -1;
                }
                current_status = atomic_load(&job->status);
                if (current_status == AV1_GPU_JOB_COMPLETE) {
                    break;
                }
                if (current_status != AV1_GPU_JOB_PROCESSING) {
                    /* Still pending, check timeout on next iteration */
                    continue;
                }
            }
        }
        
        /* Job is PROCESSING, wait for completion */
        if (current_status == AV1_GPU_JOB_PROCESSING) {
            if (timeout_us == 0) {
                while (atomic_load(&job->status) == AV1_GPU_JOB_PROCESSING) {
                    pthread_cond_wait(&gt->job_complete, &gt->mutex);
                }
            } else {
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
        
        current_status = atomic_load(&job->status);
    }
    
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
    pthread_cond_broadcast(&gt->job_available);
    pthread_mutex_unlock(&gt->mutex);
    
    pthread_join(gt->thread, NULL);
    
    pthread_cond_destroy(&gt->job_complete);
    pthread_cond_destroy(&gt->job_available);
    pthread_mutex_destroy(&gt->mutex);
    
    if (av1_mem_free) {
        av1_mem_free(gt->jobs);
        av1_mem_free(gt);
    } else {
        free(gt->jobs);
        free(gt);
    }
    
    return 0;
}

static void* gpu_thread_main(void *arg) {
    Av1GpuThread *gt = (Av1GpuThread*)arg;
    
    pthread_mutex_lock(&gt->mutex);
    
    while (!gt->exit_flag) {
        while (gt->count == 0 && !gt->exit_flag) {
            pthread_cond_wait(&gt->job_available, &gt->mutex);
        }
        
        if (gt->exit_flag) {
            break;
        }
        
        Av1GpuQueueEntry *entry = &gt->jobs[gt->head];
        Av1GpuJob *job = entry->job;
        
        gt->head = (gt->head + 1) % gt->capacity;
        gt->count--;
        entry->enqueued = 0;
        
        atomic_store(&job->status, AV1_GPU_JOB_PROCESSING);
        
        pthread_mutex_unlock(&gt->mutex);
        
        /* STUB: Simulate GPU processing */
        usleep(STUB_PROCESSING_DELAY_US);
        
        atomic_store(&job->status, AV1_GPU_JOB_COMPLETE);
        
        pthread_mutex_lock(&gt->mutex);
        pthread_cond_broadcast(&gt->job_complete);
    }
    
    pthread_mutex_unlock(&gt->mutex);
    
    return NULL;
}
