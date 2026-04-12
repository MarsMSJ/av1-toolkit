

## Progress
- [x] 1. Public API Layer — scrubbed
- [x] 2. Memory Management — scrubbed
- [x] 3. Threading Architecture — scrubbed
- [x] 4. Decode Pipeline Split (Non-Blocking DECODE) — scrubbed
- [x] 5. Queue / Pipeline Management — scrubbed
- [x] 6. Reference Frame / DPB Changes — scrubbed
- [x] 7. Bitstream Parsing Changes — scrubbed
- [x] 8. Post-Processing / Filtering — scrubbed
- [x] 9. Output / Copy Path — scrubbed
- [x] 10. Error Handling & Edge Cases — scrubbed

---

# VP9 to AV1 Decoder Diff Analysis

## 1. Public API Layer

### libvpx reference behavior

The reference implementation uses the standard libvpx decoder API:

```c
// vpx/vpx_decoder.h
vpx_codec_err_t vpx_codec_dec_init_ver(vpx_codec_ctx_t *ctx, vpx_codec_iface_t *iface,
                                        const vpx_codec_dec_cfg_t *cfg, vpx_codec_flags_t flags, int ver);
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data,
                                  unsigned int data_sz, void *user_priv, long deadline);
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter);
```

The VP9 decoder interface is accessed through `vpx_codec_iface_t` with standard capabilities:
- `VPX_CODEC_CAP_FRAME_THREADING` - frame-based multi-threading
- `VPX_CODEC_CAP_EXTERNAL_FRAME_BUFFER` - external frame buffers
- `VPX_CODEC_CAP_HIGHBITDEPTH` - 10/12-bit depth support

### Architectural pattern used

The custom implementation wraps the AOM decoder with a platform-specific API layer. This follows the **adapter pattern** - creating a thin wrapper that translates between the AOM internal interface and a custom API surface optimized for the target platform.

### Design rationale

The custom API wraps libaom to provide a platform-specific interface that:
1. **Hides AOM specifics** - Applications don't need to know about `aom_codec_ctx_t`
2. **Adds AV1-specific error codes** - OBU_ERROR and TILE_ERROR for AV1-specific failures
3. **Maintains VP9-like semantics** - Create → Decode → Sync pattern preserved
4. **Removes unsupported features** - GPU sync, thread priority, kernel affinity removed (not in AOM)

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `vpx_codec_dec_init_ver()` | `aom_codec_dec_init_ver()` |
| `vpx_codec_decode()` | `aom_codec_decode()` |
| `vpx_codec_get_frame()` | `aom_codec_get_frame()` |
| `VP9Decoder` | `aom_codec_ctx_t` (internal) |
| `VP9_COMMON` | `AV1_COMMON` (internal) |
| Custom wrapper API | `Av1AomDec*` functions |

---

## 2. Memory Management

### libvpx reference behavior

VP9 uses custom memory allocation:

```c
// vpx_mem/vpx_mem.h
void *vpx_memalign(size_t align, size_t size);
void *vpx_malloc(size_t size);
void *vpx_calloc(size_t num, size_t size);
void vpx_free(void *memblk);

// vp9_decoder.c - decoder instance allocation
VP9Decoder *vp9_decoder_create(BufferPool *const pool) {
  VP9Decoder *volatile const pbi = vpx_memalign(32, sizeof(*pbi));
  // ... frame context allocation
  CHECK_MEM_ERROR(&cm->error, cm->fc, vpx_calloc(1, sizeof(*cm->fc)));
  CHECK_MEM_ERROR(&cm->error, cm->frame_contexts, vpx_calloc(FRAME_CONTEXTS, ...));
}
```

VP9 row-based multi-threading memory:
```c
// vp9_decoder.c
void vp9_dec_alloc_row_mt_mem(RowMTWorkerData *row_mt_worker_data,
                              VP9_COMMON *cm, int num_sbs, int max_threads, int num_jobs) {
  const size_t dqcoeff_size = ((size_t)num_sbs << DQCOEFFS_PER_SB_LOG2) *
                              sizeof(*row_mt_worker_data->dqcoeff[0]);
  // Per-plane dqcoeff allocation with 32-byte alignment
  for (int plane = 0; plane < 3; ++plane) {
    CHECK_MEM_ERROR(&cm->error, row_mt_worker_data->dqcoeff[plane],
                    vpx_memalign(32, dqcoeff_size));
  }
  // EOB, partition, recon_map allocations
}
```

### Architectural pattern used

The custom implementation delegates memory management to AOM internally while exposing external frame buffer callbacks. This follows the **callback-based resource management pattern** - the decoder requests buffers from the application via callback and releases them when done.

### Design rationale

1. **Removed custom allocators** - AOM handles internal memory
2. **Simplified DPB management** - No need for custom row-MT memory (AOM handles threading internally)
3. **External frame buffer support** - Added `fb_get_cb`/`fb_release_cb` for application-provided buffers
4. **Memory query API** - Added to help applications allocate sufficient memory

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `vpx_memalign(32, ...)` | AOM internal allocation |
| `vpx_calloc()` | AOM internal allocation |
| `vp9_dec_alloc_row_mt_mem()` | Not needed - AOM handles internally |
| `BufferPool` | AOM internal frame buffer pool |
| `FrameBuffer` | `aom_image_t` (output) |

---

## 3. Threading Architecture

### libvpx reference behavior

VP9 has sophisticated multi-threading:

```c
// vp9_decoder.h
typedef struct VP9Decoder {
  VPxWorker lf_worker;           // loop filter worker
  VPxWorker *tile_workers;       // tile-level workers
  TileWorkerData *tile_worker_data;
  int num_tile_workers;
  int total_tiles;

  VP9LfSync lf_row_sync;         // loop filter sync

  int row_mt;                    // row-based multi-threading flag
  int lpf_mt_opt;                // loop filter MT optimization
  RowMTWorkerData *row_mt_worker_data;
} VP9Decoder;
```

Thread synchronization:
```c
// vp9_decoder.c - job queue for row-MT
typedef struct Job {
  int row_num;
  int tile_col;
  JobType job_type;  // PARSE_JOB, RECON_JOB, LPF_JOB
} Job;

// vp9_job_queue.h - job queue management
typedef struct JobQueueRowMt {
  Job *job_queue;
  int32_t *job_status;
  int32_t write_idx;
  int32_t read_idx;
} JobQueueRowMt;
```

Worker thread interface:
```c
// vpx_util/vpx_thread.h
typedef struct {
  void (*init)(VPxWorker *const worker);
  int (*reset)(VPxWorker *const worker);
  int (*sync)(VPxWorker *const worker);
  void (*launch)(VPxWorker *const worker);
  void (*execute)(VPxWorker *const worker);
  void (*end)(VPxWorker *const worker);
} VPxWorkerInterface;
```

### Architectural pattern used

The custom implementation removes all custom threading infrastructure and relies entirely on AOM's internal threading model. This is a **delegation pattern** - threading responsibility is delegated to the underlying codec library rather than managed at the application layer.

### Design rationale

1. **Removed custom threading** - AOM has its own internal threading model
2. **Simplified thread control** - Only `numThreads` parameter exposed to application
3. **Removed platform-specific features** - No kernel thread affinity, no priority control
4. **Tile parallelism** - AOM uses tile-based parallelism internally

### Potential Issues

**ARCHITECTURAL RISK**: The custom API doesn't expose AOM's threading behavior. If the application needs to:
- Know when decoding is actually complete
- Control thread affinity
- Handle thread-local storage

This is not possible with the current design. The sync call may block indefinitely if AOM's internal threads deadlock.

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `VPxWorker` | AOM internal threads |
| `TileWorkerData` | AOM internal tile processing |
| `JobQueueRowMt` | AOM internal job scheduling |
| `VP9LfSync` | AOM internal loop filter sync |
| `row_mt` flag | AOM internal row-MT |
| `numThreads` parameter | Passed to `aom_codec_dec_init_ver()` |

---

## 4. Decode Pipeline Split (Non-Blocking DECODE)

### libvpx reference behavior

VP9 uses a split decode/sync model:

```c
// vp9_decoder.c
int vp9_receive_compressed_data(VP9Decoder *pbi, size_t size, const uint8_t **psource) {
  // ... find free frame buffer ...
  vp9_decode_frame(pbi, source, source + size, psource);  // decode
  swap_frame_buffers(pbi);  // swap for output
  return retcode;
}

int vp9_get_raw_frame(VP9Decoder *pbi, YV12_BUFFER_CONFIG *sd, vp9_ppflags_t *flags) {
  // Returns decoded frame if available
  if (pbi->ready_for_new_data == 1) return ret;  // no frame
  *sd = *cm->frame_to_show;
  ret = 0;
}
```

The decode is blocking in `vp9_receive_compressed_data()`, but `vp9_get_raw_frame()` can be called later to retrieve the result.

### Architectural pattern used

The custom implementation uses an explicit **async decode/sync pattern** - decode work is submitted non-blocking, and a separate sync call waits for completion. This is similar to the VP9 pattern but made explicit in the API.

### Design rationale

1. **Explicit split** - Decode submits work, Sync waits for completion
2. **PTS/DTS support** - Added presentation and decode timestamps in control and result structs
3. **Status reporting** - Decode status in result indicates success/failure

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `vp9_receive_compressed_data()` | `aom_codec_decode()` |
| `vp9_get_raw_frame()` | `aom_codec_get_frame()` |
| Decode + get frame split | DecodeAu + SyncAu split |
| Blocking decode | Non-blocking with sync |

---

## 5. Queue / Pipeline Management

### libvpx reference behavior

VP9 has a job queue for row-based multi-threading:

```c
// vp9_job_queue.h
typedef struct JobQueueRowMt {
  Job *job_queue;
  int32_t *job_status;
  int32_t write_idx;
  int32_t read_idx;
  int32_t pending_jobs;
#if CONFIG_MULTITHREAD
  pthread_mutex_t mutex;
  pthread_cond_t cv;
#endif
} JobQueueRowMt;

// Job types
typedef enum JobType { PARSE_JOB, RECON_JOB, LPF_JOB } JobType;
```

Jobs are enqueued during tile parsing and dequeued by worker threads:
```c
// vp9_decodeframe.c - job enqueue during decode
void vp9_decode_frame(VP9Decoder *pbi, ...) {
  // ... parse tiles ...
  // Enqueue parse jobs
  // Enqueue recon jobs  
  // Enqueue loop filter jobs
}
```

### Architectural pattern used

The custom implementation removes the explicit job queue entirely. This follows the **implicit pipeline pattern** - the codec library manages internal parallelism without exposing job scheduling to the application.

### Design rationale

1. **Removed job queue** - AOM manages its own internal parallelism
2. **Placeholder field** - Task queue depth parameter kept for API compatibility but unused
3. **Simplified pipeline** - No application-level job scheduling

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `JobQueueRowMt` | Not exposed (AOM internal) |
| `PARSE_JOB`, `RECON_JOB`, `LPF_JOB` | AOM internal pipeline stages |
| `vp9_jobq_enqueue()` | Not needed |
| `vp9_jobq_dequeue()` | Not needed |
| `decodingTaskQueueDepth` | Placeholder, unused |

---

## 6. Reference Frame / DPB Changes

### libvpx reference behavior

VP9 manages reference frames explicitly:

```c
// vp9_decoder.c
static void swap_frame_buffers(VP9Decoder *pbi) {
  int ref_index = 0, mask;
  VP9_COMMON *const cm = &pbi->common;
  BufferPool *const pool = cm->buffer_pool;
  RefCntBuffer *const frame_bufs = cm->buffer_pool->frame_bufs;

  for (mask = pbi->refresh_frame_flags; mask; mask >>= 1) {
    const int old_idx = cm->ref_frame_map[ref_index];
    decrease_ref_count(old_idx, frame_bufs, pool);
    // ... update ref_frame_map ...
  }
  cm->frame_to_show = get_frame_new_buffer(cm);
}

// Reference counting
static INLINE void decrease_ref_count(int idx, RefCntBuffer *const frame_bufs,
                                      BufferPool *const pool) {
  if (idx >= 0 && frame_bufs[idx].ref_count > 0) {
    --frame_bufs[idx].ref_count;
    if (!frame_bufs[idx].released && frame_bufs[idx].ref_count == 0 &&
        frame_bufs[idx].raw_frame_buffer.priv) {
      pool->release_fb_cb(pool->cb_priv, &frame_bufs[idx].raw_frame_buffer);
      frame_bufs[idx].released = 1;
    }
  }
}
```

### Architectural pattern used

The custom implementation hides DPB management entirely behind the AOM interface. This is **encapsulation** - the reference frame management is an internal detail not exposed to the application.

### Design rationale

1. **Hidden DPB** - AOM manages reference frames internally
2. **Simplified tracking** - Only frame count tracked externally
3. **Removed reference controls** - No direct reference frame manipulation APIs

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `RefCntBuffer` | AOM internal frame buffer |
| `ref_frame_map[]` | AOM internal reference management |
| `swap_frame_buffers()` | AOM internal buffer swap |
| `decrease_ref_count()` | AOM internal reference counting |
| `vp9_set_reference_dec()` | Not available |
| `vp9_copy_reference_dec()` | Not available |

---

## 7. Bitstream Parsing Changes

### libvpx reference behavior

VP9 uses frame-based parsing:

```c
// vp9_decodeframe.c
void vp9_decode_frame(VP9Decoder *pbi, const uint8_t *data,
                      const uint8_t *data_end, const uint8_t **p_data_end) {
  // Read uncompressed header
  // Read compressed header  
  // Parse tiles
}

// Superframe parsing
vpx_codec_err_t vp9_parse_superframe_index(const uint8_t *data, size_t data_sz,
                                           uint32_t sizes[8], int *count,
                                           vpx_decrypt_cb decrypt_cb,
                                           void *decrypt_state) {
  // Parse superframe index to get individual frame sizes
}
```

### Architectural pattern used

The custom implementation uses OBU (OBject Unit) parsing instead of VP9's frame-based parsing. This is a **different bitstream format** - AV1 uses a hierarchical OBU structure while VP9 uses frame containers with superframes.

### Design rationale

1. **OBU-based parsing** - AV1 uses OBU structure vs VP9's frame structure
2. **Annex-B support** - Added flag to prepend start codes for streaming formats
3. **Tile group handling** - New type for continuing tile groups in streaming scenarios
4. **LEB128 decoding** - AV1 uses LEB128 for size fields (variable-length integer encoding)

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `vp9_parse_superframe_index()` | OBU parsing (custom module) |
| `vp9_decode_frame()` | `aom_decode_frame_from_obus()` |
| Frame-based parsing | OBU-based parsing |
| Superframe index | OBU header + size |
| Fixed-size frame header | LEB128-encoded sizes |

---

## 8. Post-Processing / Filtering

### libvpx reference behavior

VP9 has loop filter and post-processing:

```c
// vp9_decoder.h
typedef struct VP9Decoder {
  // ...
  VPxWorker lf_worker;
  VP9LfSync lf_row_sync;
  // ...
} VP9Decoder;

// Loop filter allocation
int vp9_alloc_loop_filter(VP9_COMMON *cm) {
  cm->lf.lfm_stride = (cm->mi_cols + (MI_BLOCK_SIZE - 1)) >> 3;
  cm->lf.lfm = (LOOP_FILTER_MASK *)vpx_calloc(...);
}

// Loop filter worker
static void loop_filter_row(VPxWorker *const worker, void *data2) {
  // Apply loop filter to a row
}
```

### Architectural pattern used

The custom implementation removes all external loop filter control. This is **internalization** - the loop filtering is handled entirely within the codec library as part of the decode pipeline.

### Design rationale

1. **Hidden loop filter** - AOM handles loop filtering internally
2. **No post-proc control** - No flags equivalent exposed
3. **Simplified API** - No loop filter parameter exposed to application

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `VPxWorker lf_worker` | Not exposed (AOM internal) |
| `VP9LfSync lf_row_sync` | Not exposed (AOM internal) |
| `vp9_loop_filter_init()` | AOM internal |
| `vp9_loop_filter_dealloc()` | AOM internal |
| Row-based loop filter threading | AOM internal |
| `vp9_ppflags_t` | Not available |

---

## 9. Output / Copy Path

### libvpx reference behavior

VP9 output uses YV12 buffer config:

```c
// vp9_decoder.c
int vp9_get_raw_frame(VP9Decoder *pbi, YV12_BUFFER_CONFIG *sd,
                      vp9_ppflags_t *flags) {
  VP9_COMMON *const cm = &pbi->common;
  // ...
  *sd = *cm->frame_to_show;
  ret = 0;
}
```

### Architectural pattern used

The custom implementation uses external buffer registration with output info retrieval. This follows the **callback-based output pattern** - the application provides buffers and is notified when output is ready.

### Design rationale

1. **External buffer support** - Allows app-provided output buffers
2. **Output info** - Frame dimensions, bit depth, profile returned in output info
3. **Planar conversion** - Converts AOM output to planar format if needed

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `YV12_BUFFER_CONFIG` | `aom_image_t` |
| `vp9_get_raw_frame()` | `aom_codec_get_frame()` |
| `cm->frame_to_show` | AOM internal output buffer |
| Output via frame buffer | Output via callback or get_frame |

---

## 10. Error Handling & Edge Cases

### libvpx reference behavior

VP9 uses detailed error handling:

```c
// vpx/vpx_codec.h
typedef enum {
  VPX_CODEC_OK,
  VPX_CODEC_ERROR,
  VPX_CODEC_MEM_ERROR,
  VPX_CODEC_ABI_MISMATCH,
  VPX_CODEC_INCAPABLE,
  VPX_CODEC_UNSUP_BITSTREAM,
  VPX_CODEC_UNSUP_FEATURE,
  VPX_CODEC_CORRUPT_FRAME,
  VPX_CODEC_INVALID_PARAM,
  VPX_CODEC_LIST_END
} vpx_codec_err_t;

// VP9-specific error info
struct vpx_internal_error_info {
  int error_code;
  int setjmp;
  jmp_buf jmp;
  char detail[80];
};

// Error handling in decoder
if (setjmp(cm->error.jmp)) {
  cm->error.setjmp = 0;
  pbi->ready_for_new_data = 1;
  release_fb_on_decoder_exit(pbi);
  return -1;
}
cm->error.setjmp = 1;
```

### Architectural pattern used

The custom implementation uses custom error codes with a mapping function from AOM errors. This is the **adapter error mapping pattern** - translating between the underlying library's error space and the wrapper's error space.

### Design rationale

1. **Custom error codes** - AV1-specific errors (OBU, TILE)
2. **Error mapping** - Function converts AOM errors to custom codes
3. **Removed setjmp** - No longjmp-based error handling (AOM handles internally)
4. **Timeout removed** - Timeout error kept for API compatibility but unused

### Applicability to AV1

| VP9 Pattern | AV1/AOM Equivalent |
|-------------|-------------------|
| `vpx_codec_err_t` | Custom return type |
| `VPX_CODEC_OK` | Custom SUCCESS code |
| `VPX_CODEC_CORRUPT_FRAME` | Custom CORRUPT_FRAME error |
| `vpx_internal_error_info` | Not needed |
| `setjmp`/`longjmp` | Not used (AOM internal) |
| `VPX_CODEC_UNSUP_BITSTREAM` | Custom UNSUP_BITSTREAM |

---

## Summary of Key Changes

| Area | VP9 Reference | AV1 Custom |
|------|---------------|------------|
| **API** | libvpx standard | Custom wrapper around AOM |
| **Memory** | Custom aligned allocator, row-MT alloc | AOM internal + external FB support |
| **Threading** | Custom workers, job queue | AOM internal only |
| **Pipeline** | Blocking decode + get_frame | DecodeAu + SyncAu split |
| **Queue** | JobQueueRowMt | Removed (AOM internal) |
| **DPB** | Explicit ref counting | AOM internal |
| **Parsing** | Frame/superframe | OBU/tile group |
| **Loop Filter** | External worker | AOM internal |
| **Output** | YV12_BUFFER_CONFIG | Custom output info + planar |
| **Errors** | setjmp + vpx_codec_err_t | Custom mapping |

---

## Potential Issues and TODOs

### Architectural Risks

1. **Thread blocking risk** - Sync call may block indefinitely if AOM's internal threads have issues
2. **No thread control** - Application cannot control or query AOM's threading
3. **Memory pressure** - No way to query AOM's current memory usage

### Incomplete Areas

1. **Task queue depth** - Placeholder field, not functional
2. **Timeout error** - Defined but never used (should be removed)
3. **Flush handling** - Not directly supported by AOM - implementation unclear

### Workarounds Required

1. **Annex-B start code** - Custom prepending in input buffer preparation
2. **External frame buffers** - Custom callback registration

### Missing from Reference

1. No equivalent to direct reference frame manipulation
2. No post-processing control flags
3. No frame metadata access (quantizers, loop filter levels)

---

## Scrub Verification
- Regex `(?i)lux_\w+` matches: 0
- Regex `(?i)sce_\w+` matches: 0
- [x] No proprietary function/struct/type names remain
- [x] No console/platform SDK names remain
- [x] No internal project names or ticket numbers remain
- [x] Report still makes technical sense for AV1 port planning