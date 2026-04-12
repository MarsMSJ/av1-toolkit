```markdown
# AV1 Internals Plan: `av1_internals.h/cpp`

## 1. `Av1AomDecoder` struct

```c
typedef enum _Av1AomDecState {
    AV1_AOM_DEC_STATE_UNINITIALIZED = 0,
    AV1_AOM_DEC_STATE_CREATED,
    AV1_AOM_DEC_STATE_DECODING,
    AV1_AOM_DEC_STATE_FLUSHING,
} Av1AomDecState;

typedef struct _Av1AomDecoder {
    aom_codec_ctx_t ctx;
    Av1AomDecStreamDesc stream_desc;
    Av1AomDecInstanceDesc instance_desc;
    Av1AomDecState state;
    
    // Frame queue / output management
    void* pending_output_buffer;      // user-provided buffer for current frame
    uint32_t pending_output_size;
    int has_pending_frame;            // true if get_frame() returned a frame but not yet consumed
    
    // External frame buffer support
    aom_get_frame_buffer_cb_fn_t fb_get_cb;
    aom_release_frame_buffer_cb_fn_t fb_release_cb;
    void* cb_priv;
    
    // Internal bookkeeping (no GPU/UCC/kernel)
    uint32_t frame_count;             // decoded frames since last reset
    int uses_annex_b;                 // if true, prepend start code before decode
} Av1AomDecoder;
```

> **Note**: All fields are generic and portable — no platform-specific types.

---

## 2. Helper functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `av1AomDecMapError` | `Av1AomDecReturnType av1AomDecMapError(aom_codec_err_t err)` | Maps AOM error codes to AV1 API codes (e.g., `AOM_CODEC_OK` → `AV1_AOM_DEC_SUCCESS`, `AOM_CODEC_CORRUPT_FRAME` → `AV1_AOM_DEC_ERR_CORRUPT_FRAME`) |
| `av1AomDecExtractStreamInfo` | `int av1AomDecExtractStreamInfo(aom_codec_ctx_t* ctx, Av1AomDecStreamDesc* desc)` | Populates `desc` from AOM stream info (w/h/profile/level) and instance config |
| `av1AomDecPrepareInputBuffer` | `aom_codec_err_t av1AomDecPrepareInputBuffer(Av1AomDecoder* dec, const uint8_t* data, size_t sz, const uint8_t** out_data, size_t* out_sz)` | If Annex-B enabled: prepend 4-byte start code and return new buffer; else pass through |
| `av1AomDecSetFrameBuffer` | `aom_codec_err_t av1AomDecSetFrameBuffer(Av1AomDecoder* dec, void* buf, size_t sz)` | Registers external output buffer via `AV1_SET_FRAME_BUFFER_SIZE_CB` if callbacks are set; otherwise uses internal fallback |
| `av1AomDecGetOutputImage` | `const aom_image_t* av1AomDecGetOutputImage(Av1AomDecoder* dec, aom_codec_iter_t* iter)` | Wrapper around `aom_codec_get_frame()` with state validation |

---

## 3. Dropped elements from VP9 internals

| VP9 Element | Reason for Removal |
|-------------|--------------------|
| `pSoftwareDecoderContext` (VPX context pointer) | Replaced by direct `aom_codec_ctx_t` storage |
| `swThreadAffinityMask`, `copyThreadAffinityMask` | No kernel CPU affinity in AOM decoder — thread count is set via config |
| `recommendedSwSchedulePriority`, `recommendedCopyThreadPriority` | Priority control not exposed in AOM API |
| GPU/UCC/AGC context types (`SceUnifiedComputeContextType`, etc.) | AV1 reference decoder has no GPU compute integration |
| `GPU_SYNC_OVERRIDE`, `GPU_SYNC_EVENT`, `GPU_INTERRUPT` macros | No GPU sync semantics in open-source AOM |
| `LuxPrDecoder::frame_queue` (VP9-specific queue) | AOM uses iterator pattern — no explicit queue needed |
| `LuxPrDecoder::numFrames` field | AV1 has no superframes; each AU is one frame |
| `LuxPrDecoder::dpbFrameNum` → replaced by `instance_desc.maxExtraDisplayFrameNum` | AOM manages DPB internally via context config |

> **Note**: All dropped elements were platform-specific or VP9-internal — none are required for AV1 portability.
```