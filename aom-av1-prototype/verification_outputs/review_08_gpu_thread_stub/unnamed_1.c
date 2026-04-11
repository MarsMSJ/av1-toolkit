// Current behavior:
int current_status = atomic_load(&job->status);
if (current_status == AV1_GPU_JOB_COMPLETE) {
    return 0;  // OK
}
if (current_status == AV1_GPU_JOB_PROCESSING) {
    // Wait for completion...
} else {
    // PENDING case: falls through and returns -1 IMMEDIATELY
}
