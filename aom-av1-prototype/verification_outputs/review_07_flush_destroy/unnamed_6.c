static void *worker_thread_func(void *arg) {
    // ...
    printf("Worker thread %d started\n", worker->thread_id);
    set_thread_priority(worker->thread, &worker->config);
    worker->running = true;  // Set AFTER prints
    printf("Worker thread %d exiting\n", worker->thread_id);
    return NULL;
}
