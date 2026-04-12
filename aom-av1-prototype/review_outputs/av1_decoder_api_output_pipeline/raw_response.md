

I'll review the focus areas for the av1_decoder_api.c file (lines 1048–1292).

## Progress
- [x] Focus area 1: av1_sync timeout=0 inconsistency — checked
- [x] Focus area 2: av1_set_output copy job correctness — checked
- [x] Focus area 3: av1_receive_output ref_count handling — checked
- [x] Focus area 4: Pending output overflow handling — checked

---

## Focus Area 1: av1_sync timeout=0 inconsistency

### Bug 1.1: Timeout semantics mismatch between API and implementation
- **File**: av1_decoder_api.c, line ~1051
- **Problem**: The API documentation states that `timeout_us=0` means "infinite wait", but the underlying `av1_frame_queue_pop` treats 0 as non-blocking (as shown in the legacy `av1_get_decoded_frame` function which says "0 = non-blocking"). This causes a deadlock when users call `av1_sync(decoder, 0, &output)` expecting infinite wait but the queue returns immediately with `AV1_NEED_MORE_DATA`.
- **Fix**: Map 0 to UINT32_MAX before passing to the queue, or add a special-case loop for infinite wait.

---

## Focus Area 2: av1_set_output copy job correctness

### Bug 2.1: Plane dimensions stored as pixel counts instead of byte counts
- **File**: av1_decoder_api.c, line ~1127-1132
- **Problem**: The code stores `output_buffer->widths` and `heights` (in pixels) directly into `copy_job->plane_widths` and `plane_heights`. According to the header comment in av1_copy_thread.h, these fields should be "width in bytes per plane, rows per plane". For 10-bit or 12-bit content, each pixel is 2 bytes, so the byte width should be `pixel_width * 2`. This causes incorrect copy sizes for high bit-depth content.
- **Fix**: Multiply widths by the appropriate byte factor based on bit depth (1 for 8-bit, 2 for 10/12-bit).

### Bug 2.2: Bit depth not used when building copy job
- **File**: av1_decoder_api.c, line ~1108-1132
- **Problem**: The code never reads `output_buffer->bit_depth` to determine the bytes per pixel. It also doesn't validate or use the DPB slot's bit depth. Without this, the copy job cannot correctly compute the data size to copy.
- **Fix**: Read `output_buffer->bit_depth` and compute byte widths as `(bit_depth > 8) ? (width * 2) : width`.

---

## Focus Area 3: av1_receive_output ref_count handling

### Bug 3.1: Potential race condition in av1_release_frame
- **File**: av1_decoder_api.c, line ~1260-1270
- **Problem**: `av1_release_frame` releases the DPB slot immediately without waiting for any pending copy job to complete. If a copy job is in progress (status = AV1_COPY_IN_PROGRESS), releasing the DPB slot could cause the slot to be reused while the copy thread is still reading from it, leading to data corruption or use-after-free.
- **Fix**: Check if there's a copy job and wait for it to complete before releasing the DPB slot, similar to how `av1_receive_output` does it.

---

## Focus Area 4: Pending output overflow handling

### Bug 4.1: Missing AV1_ERROR_QUEUE_FULL return code
- **File**: av1_decoder_api.c, line ~1078-1082
- **Problem**: When `add_pending_output` fails (likely because the pending output table is full), the function returns generic `AV1_ERROR` instead of the specific `AV1_ERROR_QUEUE_FULL` error code. This makes it impossible for callers to distinguish between a full pending output table and other errors.
- **Fix**: Check the return value of `add_pending_output` and return `AV1_ERROR_QUEUE_FULL` when it fails.

---

## Corrected File

### av1_decoder_api.c

```c
// [lines 1048–1292 of 1292]
Av1DecodeResult av1_sync(Av1Decoder *decoder, 
                         uint32_t timeout_us,
                         Av1DecodeOutput *out_result) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if we're in flushing state and queue is empty
    if (decoder->state == AV1_DECODER_STATE_FLUSHING) {
        if (av1_frame_queue_count(&decoder->ready_queue) == 0) {
            return AV1_END_OF_STREAM;
        }
    }
    
    // FIX Bug 1.1: Map timeout=0 (infinite wait) to UINT32_MAX
    // The API doc says 0 means infinite wait, but the queue treats 0 as non-blocking
    uint32_t queue_timeout = (timeout_us == 0) ? UINT32_MAX : timeout_us;
    
    // Pop from ready queue
    Av1FrameEntry entry;
    int result = av1_frame_queue_pop(&decoder->ready_queue, &entry, queue_timeout);
    
    if (result != 0) {
        // Timeout or error
        if (decoder->state == AV1_DECODER_STATE_FLUSHING) {
            return AV1_END_OF_STREAM;
        }
        return AV1_NEED_MORE_DATA;
    }
    
    // FIX Bug 4.1: Check add_pending_output result and return proper error code
    // Add to pending output table
    if (add_pending_output(decoder, entry.frame_id, entry.dpb_slot) != 0) {
        fprintf(stderr, "av1_sync: failed to add to pending output (table full?)\n");
        // Put it back in queue
        av1_frame_queue_push(&decoder->ready_queue, &entry);
        return AV1_ERROR_QUEUE_FULL;
    }
    
    // Fill output result
    if (out_result) {
        out_result->frame_ready = 1;
        out_result->frame_id = entry.frame_id;
        out_result->show_existing_frame = entry.show_existing_frame;
        out_result->dpb_slot = entry.dpb_slot;
    }
    
    printf("av1_sync: frame_id=%u, dpb_slot=%d\n", entry.frame_id, entry.dpb_slot);
    
    return AV1_OK;
}

// ============================================================================
// av1_set_output Implementation
// ============================================================================

Av1DecodeResult av1_set_output(Av1Decoder *decoder, 
                               uint32_t frame_id,
                               const Av1OutputBuffer *output_buffer) {
    if (!decoder || !output_buffer) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find pending output entry
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (!pending) {
        fprintf(stderr, "av1_set_output: frame_id %u not found in pending output\n", frame_id);
        return AV1_ERROR;
    }
    
    // Get DPB slot
    int dpb_slot = pending->dpb_slot;
    if (dpb_slot < 0 || dpb_slot >= decoder->dpb_count) {
        fprintf(stderr, "av1_set_output: invalid dpb_slot %d\n", dpb_slot);
        return AV1_ERROR;
    }
    
    Av1DPBSlot *slot = &decoder->dpb[dpb_slot];
    if (!slot->in_use) {
        fprintf(stderr, "av1_set_output: DPB slot %d not in use\n", dpb_slot);
        return AV1_ERROR;
    }
    
    // Allocate copy job
    Av1CopyJob *copy_job = (Av1CopyJob *)malloc(sizeof(Av1CopyJob));
    if (!copy_job) {
        fprintf(stderr, "av1_set_output: failed to allocate copy job\n");
        return AV1_ERROR;
    }
    
    // Build copy job from DPB slot to output buffer
    memset(copy_job, 0, sizeof(Av1CopyJob));
    
    copy_job->frame_id = frame_id;
    copy_job->dpb_slot = dpb_slot;
    
    // Source (DPB)
    copy_job->src_planes[0] = slot->planes[0];
    copy_job->src_planes[1] = slot->planes[1];
    copy_job->src_planes[2] = slot->planes[2];
    copy_job->src_strides[0] = slot->strides[0];
    copy_job->src_strides[1] = slot->strides[1];
    copy_job->src_strides[2] = slot->strides[2];
    
    // Destination (output buffer)
    copy_job->dst_planes[0] = output_buffer->planes[0];
    copy_job->dst_planes[1] = output_buffer->planes[1];
    copy_job->dst_planes[2] = output_buffer->planes[2];
    copy_job->dst_strides[0] = output_buffer->strides[0];
    copy_job->dst_strides[1] = output_buffer->strides[1];
    copy_job->dst_strides[2] = output_buffer->strides[2];
    
    // FIX Bug 2.1 & 2.2: Compute byte widths based on bit depth
    // The plane_widths/plane_heights should be in bytes, not pixels
    // For 8-bit: 1 byte per pixel
    // For 10/12-bit: 2 bytes per pixel
    int bytes_per_pixel = (output_buffer->bit_depth > 8) ? 2 : 1;
    
    copy_job->plane_widths[0] = output_buffer->widths[0] * bytes_per_pixel;
    copy_job->plane_widths[1] = output_buffer->widths[1] * bytes_per_pixel;
    copy_job->plane_widths[2] = output_buffer->widths[2] * bytes_per_pixel;
    copy_job->plane_heights[0] = output_buffer->heights[0];
    copy_job->plane_heights[1] = output_buffer->heights[1];
    copy_job->plane_heights[2] = output_buffer->heights[2];
    
    // Enqueue to copy thread
    if (av1_copy_thread_enqueue(decoder->copy_thread, copy_job) != 0) {
        fprintf(stderr, "av1_set_output: failed to enqueue copy job\n");
        free(copy_job);
        return AV1_ERROR;
    }
    
    // Store copy job in pending output
    pending->copy_job = copy_job;
    pending->ref_count++;
    
    printf("av1_set_output: frame_id=%u, enqueued copy job\n", frame_id);
    
    return AV1_OK;
}

// ============================================================================
// av1_receive_output Implementation
// ============================================================================

Av1DecodeResult av1_receive_output(Av1Decoder *decoder, 
                                    uint32_t frame_id,
                                    uint32_t timeout_us) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find pending output entry
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (!pending) {
        fprintf(stderr, "av1_receive_output: frame_id %u not found\n", frame_id);
        return AV1_ERROR;
    }
    
    // If no copy job (frame not set for output), just release DPB
    if (!pending->copy_job) {
        // Release DPB reference
        release_dpb_slot(decoder, pending->dpb_slot);
        remove_pending_output(decoder, frame_id);
        return AV1_OK;
    }
    
    // Wait for copy job to complete
    int wait_result = av1_copy_thread_wait(decoder->copy_thread, pending->copy_job, timeout_us);
    
    if (wait_result != 0) {
        fprintf(stderr, "av1_receive_output: timeout waiting for copy job\n");
        return AV1_ERROR;
    }
    
    // Check copy status
    int status = av1_copy_thread_get_status(pending->copy_job);
    if (status != AV1_COPY_COMPLETE) {
        fprintf(stderr, "av1_receive_output: copy job failed (status=%d)\n", status);
        return AV1_ERROR;
    }
    
    // Release DPB reference
    release_dpb_slot(decoder, pending->dpb_slot);
    
    // Free copy job
    free(pending->copy_job);
    pending->copy_job = NULL;
    
    // Remove from pending output table
    remove_pending_output(decoder, frame_id);
    
    printf("av1_receive_output: frame_id=%u completed\n", frame_id);
    
    return AV1_OK;
}

// Legacy functions
Av1DecodeResult av1_get_decoded_frame(Av1Decoder *decoder, 
                                       Av1FrameEntry *out_entry,
                                       uint32_t timeout_us) {
    if (!decoder || !out_entry) {
        return AV1_INVALID_PARAM;
    }
    
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    int result = av1_frame_queue_pop(&decoder->ready_queue, out_entry, timeout_us);
    
    if (result == 0) {
        decoder->frames_output++;
        return AV1_OK;
    }
    
    return AV1_ERROR;
}

Av1DecodeResult av1_release_frame(Av1Decoder *decoder, uint32_t frame_id) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find and release pending output
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (pending) {
        // FIX Bug 3.1: Wait for copy job to complete before releasing DPB slot
        // If there's an in-progress copy job, we must wait for it to finish
        // to avoid use-after-free of the DPB buffer
        if (pending->copy_job) {
            // Wait for copy to complete (infinite wait since we're releasing resources)
            int wait_result = av1_copy_thread_wait(decoder->copy_thread, pending->copy_job, UINT32_MAX);
            if (wait_result != 0) {
                fprintf(stderr, "av1_release_frame: warning - copy job did not complete\n");
                // Continue anyway to avoid leak, but don't free copy_job
            } else {
                // Check status and free copy job only if successful
                int status = av1_copy_thread_get_status(pending->copy_job);
                if (status == AV1_COPY_COMPLETE) {
                    free(pending->copy_job);
                } else {
                    fprintf(stderr, "av1_release_frame: warning - copy job failed (status=%d)\n", status);
                }
            }
            pending->copy_job = NULL;
        }
        
        release_dpb_slot(decoder, pending->dpb_slot);
        remove_pending_output(decoder, frame_id);
    }
    
    return AV1_OK;
}
```