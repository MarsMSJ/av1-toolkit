#include "av1_job_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define CAPACITY 8
#define NUM_ENTRIES 1000

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); tests_passed++; } \
    else { printf("  FAIL: %s\n", msg); tests_failed++; } \
} while(0)

static void test_basic_capacity(void) {
    printf("Test 1: Basic capacity test\n");
    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    
    ASSERT(av1_frame_queue_init(&q, storage, CAPACITY) == 0, "Queue init");
    
    for (int i = 0; i < CAPACITY; i++) {
        Av1FrameEntry e = { .frame_id = static_cast<uint32_t>(i), .dpb_slot = i, .show_frame = 1, .show_existing_frame = 0 };
        ASSERT(av1_frame_queue_push(&q, &e) == 0, "Push entry");
    }
    
    Av1FrameEntry overflow = { .frame_id = 99, .dpb_slot = 99, .show_frame = 1, .show_existing_frame = 0 };
    ASSERT(av1_frame_queue_push(&q, &overflow) == -1, "9th push fails (full)");
    ASSERT(av1_frame_queue_is_full(&q) == 1, "Queue reports full");
    
    av1_frame_queue_destroy(&q);
}

static void test_fifo_order(void) {
    printf("Test 2: FIFO order test\n");
    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    av1_frame_queue_init(&q, storage, CAPACITY);
    
    for (int i = 0; i < CAPACITY; i++) {
        Av1FrameEntry e = { .frame_id = static_cast<uint32_t>(i * 10), .dpb_slot = i, .show_frame = i % 2, .show_existing_frame = 0 };
        av1_frame_queue_push(&q, &e);
    }
    
    Av1FrameEntry out;
    for (int i = 0; i < 3; i++) {
        ASSERT(av1_frame_queue_pop(&q, &out, 0) == 0, "Pop entry");
        ASSERT(out.frame_id == (uint32_t)(i * 10), "Correct frame_id order");
    }
    
    av1_frame_queue_destroy(&q);
}

static void test_timeout_empty(void) {
    printf("Test 3: Timeout on empty queue\n");
    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    av1_frame_queue_init(&q, storage, CAPACITY);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    Av1FrameEntry out;
    int result = av1_frame_queue_pop(&q, &out, 100000);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    ASSERT(result == -1, "Pop returns -1 on timeout");
    
    long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_nsec - start.tv_nsec) / 1000;
    if (elapsed_us >= 90000 && elapsed_us <= 200000) {
        printf("  PASS: Timeout elapsed ~100ms (got %ldus)\n", elapsed_us);
        tests_passed++;
    } else {
        printf("  FAIL: Timeout elapsed %ldus\n", elapsed_us);
        tests_failed++;
    }
    
    av1_frame_queue_destroy(&q);
}

typedef struct {
    Av1FrameQueue *q;
    int num_entries;
    int delay_us;
    int *errors;
} ThreadData;

static void* producer_thread(void *arg) {
    ThreadData *data = (ThreadData*)arg;
    for (int i = 0; i < data->num_entries; i++) {
        Av1FrameEntry e = { .frame_id = (uint32_t)i, .dpb_slot = i % 16, .show_frame = i % 2, .show_existing_frame = 0 };
        if (av1_frame_queue_push(data->q, &e) != 0) {
            *(data->errors) = 1;
            return NULL;
        }
        if (data->delay_us > 0) usleep(data->delay_us);
    }
    return NULL;
}

static void* consumer_thread(void *arg) {
    ThreadData *data = (ThreadData*)arg;
    for (int i = 0; i < data->num_entries; i++) {
        Av1FrameEntry out;
        if (av1_frame_queue_pop(data->q, &out, 500000) != 0) {
            *(data->errors) = 1;
            return NULL;
        }
        if (out.frame_id != (uint32_t)i) {
            *(data->errors) = 1;
        }
        if (data->delay_us > 0) usleep(data->delay_us);
    }
    return NULL;
}

static void test_two_thread(void) {
    printf("Test 4: Two-thread producer/consumer test\n");
    Av1FrameEntry storage[NUM_ENTRIES];
    Av1FrameQueue q;
    av1_frame_queue_init(&q, storage, NUM_ENTRIES);
    
    pthread_t producer, consumer;
    int prod_errors = 0, cons_errors = 0;
    
    ThreadData prod_data = { &q, NUM_ENTRIES, 0, &prod_errors };
    ThreadData cons_data = { &q, NUM_ENTRIES, 0, &cons_errors };
    
    pthread_create(&consumer, NULL, consumer_thread, &cons_data);
    usleep(1000);
    pthread_create(&producer, NULL, producer_thread, &prod_data);
    
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);
    
    ASSERT(prod_errors == 0, "Producer had no errors");
    ASSERT(cons_errors == 0, "Consumer had no errors");
    ASSERT(av1_frame_queue_count(&q) == 0, "Queue empty after both threads done");
    
    av1_frame_queue_destroy(&q);
}

int main(void) {
    printf("=== AV1 Job Queue Tests ===\n\n");
    
    test_basic_capacity();
    printf("\n");
    test_fifo_order();
    printf("\n");
    test_timeout_empty();
    printf("\n");
    test_two_thread();
    printf("\n");
    
    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
