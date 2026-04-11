#ifndef AV1_JOB_QUEUE_H
#define AV1_JOB_QUEUE_H

#include <stdint.h>
#include <pthread.h>

typedef struct Av1FrameEntry {
    uint32_t frame_id;
    int      dpb_slot;
    int      show_frame;
    int      show_existing_frame;
} Av1FrameEntry;

typedef struct Av1FrameQueue {
    Av1FrameEntry *entries;
    int            capacity;
    int            head;
    int            tail;
    int            count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} Av1FrameQueue;

int av1_frame_queue_init(Av1FrameQueue *q, Av1FrameEntry *storage, int capacity);
int av1_frame_queue_push(Av1FrameQueue *q, const Av1FrameEntry *entry);
int av1_frame_queue_pop(Av1FrameQueue *q, Av1FrameEntry *out, uint32_t timeout_us);
int av1_frame_queue_count(Av1FrameQueue *q);
int av1_frame_queue_is_full(Av1FrameQueue *q);
void av1_frame_queue_destroy(Av1FrameQueue *q);

#endif /* AV1_JOB_QUEUE_H */
