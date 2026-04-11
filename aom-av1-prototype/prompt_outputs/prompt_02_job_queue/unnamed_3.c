#include "av1_job_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define CAPACITY 8
#define NUM_ENTRIES 1000

// Test result tracking
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

// Test 1: Basic push/pop with capacity
static void test_basic_capacity(void) {
    printf("Test 1: Basic capacity test\n");

    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    
    ASSERT(av1_frame_queue_init(&q, storage, CAPACITY) == 0, "Queue init");

    // Push 8 entries
    for (int i = 0; i < CAPACITY; i++) {
        Av1FrameEntry e = { .frame_id = i, .dpb_slot = i, .show_frame = 1, .show_existing_frame = 0 };
        ASSERT(av1_frame_queue_push(&q, &e) == 0, "Push entry");
    }

    // 9th push should fail (queue full)
    Av1FrameEntry overflow = { .frame_id = 99, .dpb_slot = 99, .show_frame = 1, .show_existing_frame = 0 };
    ASSERT(av1_frame_queue_push(&q, &overflow) == -1, "9th push fails (full)");

    ASSERT(av1_frame_queue_is_full(&q) == 1, "Queue reports full");
    ASSERT(av1_frame_queue_count(&q) == CAPACITY, "Count equals capacity");

    av1_frame_queue_destroy(&q);
}

// Test 2: FIFO order after pops
static void test_fifo_order(void) {
    printf("Test 2: FIFO order test\n");

    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    
    av1_frame_queue_init(&q, storage, CAPACITY);

    // Push 8 entries
    for (int i = 0; i < CAPACITY; i++) {
        Av1FrameEntry e = { .frame_id = i * 10, .dpb_slot = i, .show_frame = i % 2, .show_existing_frame = 0 };
        av1_frame_queue_push(&q, &e);
    }

    // Pop 3 entries and verify order
    Av1FrameEntry out;
    for (int i = 0; i < 3; i++) {
        ASSERT(av1_frame_queue_pop(&q, &out, 0) == 0, "Pop entry");
        ASSERT(out.frame_id == (uint32_t)(i * 10), "Correct frame_id order");
        ASSERT(out.dpbslot == i, "Correct dpb_slot order");
    }

    // Push 3 more
    for (int i = 0; i < 3; i++) {
        Av1FrameEntry e = { .frame_id = 80 + i, .dpb_slot = 8 + i, .show_frame = 1, .show_existing_frame = 0 };
        ASSERT(av1_frame_queue_push(&q, &e) == 0, "Push new entry");
    }

    // Pop all 8 and verify order
    int expected_ids[] = {30, 40, 50, 60, 70, 80, 81, 82};
    for (int i = 0; i < 8; i++) {
        ASSERT(av1_frame_queue_pop(&q, &out, 0) == 0, "Pop remaining entry");
        ASSERT(out.frame_id == expected_ids[i], "Correct frame_id after mixed ops");
    }

    ASSERT(av1_frame_queue_count(&q) == 0, "Queue empty after all pops");

    av1_frame_queue_destroy(&q);
}

// Test 3: Timeout on empty queue
static void test_timeout_empty(void) {
    printf("Test 3: Timeout on empty queue\n");

    Av1FrameEntry storage[CAPACITY];
    Av1FrameQueue q;
    
    av1_frame_queue_init(&q, storage, CAPACITY);

    // Try to pop from empty queue with 100ms timeout
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    Av1FrameEntry out;
    int result = av1_frame_queue_pop(&q, &out, 100000); // 100ms = 100000us

    clock_gettime(CLOCK_MONOTONIC, &end);

    ASSERT(result == -1, "Pop returns -1 on timeout");

    // Verify elapsed time is approximately 100ms (allow some tolerance)
    long elapsed_us = (end.tv_sec - start.tv_sec) * USEC_PER_SEC + 
                      (end.tv_nsec - start.tv_nsec) / 1000;
    
    if (elapsed_us >= 90000 && elapsed_us <= 200000) {
        printf("  PASS: Timeout elapsed ~100ms (got %ldus)\n", elapsed_us);
        tests_passed++;
    } else {
        printf("  FAIL: Timeout elapsed %ldus (expected ~100000us)\n", elapsed_us);
        tests_failed++;
    }

    av1_frame_queue_destroy(&q);
}

// Thread data for producer/consumer test
typedef struct {
    Av1FrameQueue *q;
    int num_entries;
    int delay_us;
    int *errors;
} ThreadData;

// Producer thread function
static void* producer_thread(void *arg) {
    ThreadData *data = (ThreadData*)arg;
    
    for (int i = 0; i < data->num_entries; i++) {
        Av1FrameEntry e = { 
            .frame_id = (uint32_t)i, 
            .dpb_slot = i % 16, 
            .show_frame = i % 2, 
            .show_existing_frame = 0 
        };
        
        int ret = av1_frame_queue_push(data->q, &e);
        if (ret != 0) {
            *(data->errors) = 1;
            return NULL;
        }
        
        if (data->delay_us > 0) {
            usleep(data->delay_us);
        }
    }
    
    return NULL;
}

// Consumer thread function
static void* consumer_thread(void *arg) {
    ThreadData *data = (ThreadData*)arg;
    
    for (int i = 0; i < data->num_entries; i++) {
        Av1FrameEntry out;
        int ret = av1_frame_queue_pop(data->q, &out, 500000); // 500ms timeout
        
        if (ret != 0) {
            printf("Consumer: pop failed at index %d\n", i);
            *(data->errors) = 1;
            return NULL;
        }
        
        // Verify order
        if (out.frame_id != (uint32_t)i) {
            printf("Consumer: expected frame_id %d, got %u\n", i, out.frame_id);
            *(data->errors) = 1;
            return NULL;
        }
        
        if (data->delay_us > 0) {
            usleep(data->delay_us);
        }
    }
    
    return NULL;
}

// Test 4: Two-thread producer/consumer
static void test_two_thread(void) {
    printf("Test 4: Two-thread producer/consumer test\n");

    Av1FrameEntry storage[NUM_ENTRIES];
    Av1FrameQueue q;
    
    ASSERT(av1_frame_queue_init(&q, storage, NUM_ENTRIES) == 0, "Queue init for threads");

    pthread_t producer, consumer;
    ThreadData prod_data = { &q, NUM_ENTRIES, 0, malloc(sizeof(int)) };
    ThreadData cons_data = { &q, NUM_ENTRIES, 0, malloc(sizeof(int)) };
    *prod_data.errors = 0;
    *cons_data.errors = 0;

    // Start consumer first (will block until producer pushes)
    pthread_create(&consumer, NULL, consumer_thread, &cons_data);
    
    // Small delay to let consumer start
    usleep(1000);
    
    // Start producer
    pthread_create(&producer, NULL, producer_thread, &prod_data);

    // Wait for both to complete
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    ASSERT(*prod_data.errors == 0, "Producer had no errors");
    ASSERT(*cons_data.errors == 0, "Consumer had no errors");
    ASSERT(av1_frame_queue_count(&q) == 0, "Queue empty after both threads done");

    free(prod_data.errors);
    free(cons_data.errors);
    av1_frame_queue_destroy(&q);
}

// Test 5: Concurrent push/pop with delays
static void test_concurrent_with_delays(void) {
    printf("Test 5: Concurrent producer/consumer with small delays\n");

    Av1FrameEntry storage[NUM_ENTRIES];
    Av1FrameQueue q;
    
    av1_frame_queue_init(&q, storage, NUM_ENTRIES);

    pthread_t producer, consumer;
    int prod_errors = 0, cons_errors = 0;
    
    ThreadData prod_data = { &q, NUM_ENTRIES, 50, &prod_errors };  // 50us delay
    ThreadData cons_data = { &q, NUM_ENTRIES, 50, &cons_errors };  // 50us delay

    pthread_create(&consumer, NULL, consumer_thread, &cons_data);
    usleep(1000);
    pthread_create(&producer, NULL, producer_thread, &prod_data);

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    ASSERT(prod_errors == 0, "Producer had no errors with delays");
    ASSERT(cons_errors == 0, "Consumer had no errors with delays");
    ASSERT(av1_frame_queue_count(&q) == 0, "Queue empty after concurrent test");

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

    test_concurrent_with_delays();
    printf("\n");

    printf("=== Test Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
