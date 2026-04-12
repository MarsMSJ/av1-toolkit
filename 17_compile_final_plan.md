```markdown
# AV1 AOM Decoder API Layer — Implementation Plan

## 1. Overview

This API layer provides a consistent, platform-agnostic interface for AV1 decoding that mirrors the structure and semantics of the existing VP9 custom decoder API (`sce_prvp9dec.h`). It sits atop the **AOMedia AV1 reference decoder** (`libaom`) and abstracts away low-level codec-specific details while preserving high-level control flow (create → submit AU → sync output → destroy).

- **Purpose**: Enable reuse of existing VP9-based decoding infrastructure with minimal changes when migrating to AV1.
- **Underlying library**: `libaom` — specifically `aom_codec_dec_init_ver`, `aom_codec_decode`, `aom_codec_get_frame`, and control interfaces.
- **Design philosophy**:
  - Preserve familiar function signatures (e.g., `av1AomDecDecodeAu`, `av1AomDecSyncAu`)
  - Retain VP9-style state management (`CREATED`, `DECODING`, `FLUSHING`)
  - Remove all platform-specific extensions (GPU, kernel, PRX) — AV1 decoder is pure software
  - Add AV1-native features: OBU parsing, Annex B support, tile threading config

## 2. Header: av1_aom_dec.h

```c
/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2-Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2-Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software.
 */

#ifndef _AV1_AOM_DEC_H_
#define _AV1_AOM_DEC_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
typedef int Av1AomDecReturnType;

enum _Av1AomDecReturnType {
    AV1_AOM_DEC_SUCCESS = 0,
    AV1_AOM_DEC_ERR_INVALID_PARAM = -1,
    AV1_AOM_DEC_ERR_INVALID_CTX = -2,
    AV1_AOM_DEC_ERR_MEM_ALLOC = -3,
    AV1_AOM_DEC_ERR_UNSUP_BITSTREAM = -4,
    AV1_AOM_DEC_ERR_UNSUP_FEATURE = -5,
    AV1_AOM_DEC_ERR_CORRUPT_FRAME = -6,
    AV1_AOM_DEC_ERR_TIMEOUT = -7,  /* kept for compatibility but unused in AOM */
    AV1_AOM_DEC_ERR_BUF_TOO_SMALL = -8,
    AV1_AOM_DEC_ERR_OBU_ERROR = -25,
    AV1_AOM_DEC_ERR_TILE_ERROR = -26,
};

/* Enumerations */
typedef enum _Av1AomDecProfile {
    AV1_AOM_DEC_PROFILE_MAIN = 0,
    AV1_AOM_DEC_PROFILE_HIGH = 1,
    AV1_AOM_DEC_PROFILE_PROFESSIONAL = 2,
} Av1AomDecProfile;

typedef enum _Av1AomDecLevel {
    AV1_AOM_DEC_LEVEL_2_0 = 0,
    AV1_AOM_DEC_LEVEL_2_1 = 1,
    AV1_AOM_DEC_LEVEL_3_0 = 2,
    AV1_AOM_DEC_LEVEL_3_1 = 3,
    AV1_AOM_DEC_LEVEL_4_0 = 4,
    AV1_AOM_DEC_LEVEL_4_1 = 5,
    AV1_AOM_DEC_LEVEL_5_0 = 6,
    AV1_AOM_DEC_LEVEL_5_1 = 7,
    AV1_AOM_DEC_LEVEL_5_2 = 8,
    AV1_AOM_DEC_LEVEL_6_0 = 9,
    AV1_AOM_DEC_LEVEL_6_1 = 10,
    AV1_AOM_DEC_LEVEL_6_2 = 11,
    AV1_AOM_DEC_LEVEL_7_0 = 12,
    AV1_AOM_DEC_LEVEL_7_1 = 13,
    AV1_AOM_DEC_LEVEL_7_2 = 14,
} Av1AomDecLevel;

typedef enum _Av1AomDecColorSpace {
    AV1_AOM_DEC_COLOR_SPACE_BT_601 = 0,
    AV1_AOM_DEC_COLOR_SPACE_BT_709 = 1,
    AV1_AOM_DEC_COLOR_SPACE_SRGB = 2,
    AV1_AOM_DEC_COLOR_SPACE_BT_2020_NCL = 4,
    AV1_AOM_DEC_COLOR_SPACE_BT_2020_CL = 5,
    AV1_AOM_DEC_COLOR_SPACE_UNSPECIFIED = 6,
} Av1AomDecColorSpace;

typedef enum _Av1AomDecPictureFormat {
    AV1_AOM_DEC_PIC_FMT_YUV420_PLANAR = 0,
    AV1_AOM_DEC_PIC_FMT_NV12 = 1,
    AV1_AOM_DEC_PIC_FMT_YUV420_10BIT_PLANAR = 2,
} Av1AomDecPictureFormat;

/* Structs */
typedef struct _Av1AomDecMemoryDesc {
    void* pBase;
    uint32_t size;
    uint32_t alignment;
} Av1AomDecMemoryDesc;

typedef enum _Av1AomDecInstanceMemoryType {
    AV1_AOM_DEC_INSTANCE_MEMORY_CPU_ONLY = 0,
    AV1_AOM_DEC_INSTANCE_MEMORY_CPU_GPU_SHARED = 1,
} Av1AomDecInstanceMemoryType;

typedef struct _Av1AomDecInstanceMemoryDesc {
    Av1AomDecInstanceMemoryType type;
    uint32_t size;
} Av1AomDecInstanceMemoryDesc;

typedef struct _Av1AomDecStreamDesc {
    Av1AomDecProfile profile;
    Av1AomDecLevel level;
    uint32_t maxFrameWidth;
    uint32_t maxFrameHeight;
    uint32_t dpbFrameNum;
    uint32_t pixelRowByteAlignment;
    uint32_t maxPixelBitDepth;
    int usesAnnexB;
    int pixelMsbAlignFlag;
} Av1AomDecStreamDesc;

typedef struct _Av1AomDecInstanceDesc {
    Av1AomDecMemoryDesc memory;
    Av1AomDecInstanceMemoryType memoryType;
    uint32_t maxExtraDisplayFrameNum;
    uint32_t decodingTaskQueueDepth;
    int autoSyncFlag;
    uint32_t numThreads;
    uint32_t decoderVersion;
} Av1AomDecInstanceDesc;

typedef struct _Av1AomDecInstanceCharacteristics {
    uint32_t maxSupportedWidth;
    uint32_t maxSupportedHeight;
    uint32_t maxDpbSize;
    uint32_t minFrameRate;
    uint32_t maxFrameRate;
} Av1AomDecInstanceCharacteristics;

typedef struct _Av1AomDecAuDecodingControl {
    void* callerData;
    uint64_t ptsData;
    uint64_t dtsData;
} Av1AomDecAuDecodingControl;

typedef struct _Av1AomDecAuResult {
    int decodeStatus;
    uint32_t decodedFrameNum;
    uint64_t ptsData;
    uint64_t dtsData;
} Av1AomDecAuResult;

typedef struct _Av1AomDecOutputInfoBuffer {
    Av1AomDecProfile profile;
    Av1AomDecLevel level;
    uint32_t frameWidth;
    uint32_t frameHeight;
    uint32_t pixelRowByteAlignment;
    uint32_t maxPixelBitDepth;
    int usesAnnexB;
    int pixelMsbAlignFlag;
} Av1AomDecOutputInfoBuffer;

/* Constants */
#define AV1_AOM_DEC_INVALID_TIMESTAMP UINT64_MAX

/* Function declarations */
extern Av1AomDecReturnType av1AomDecGetVersion(uint32_t* pVersion);

extern Av1AomDecReturnType av1AomDecQueryMemory(
    const Av1AomDecStreamDesc* pStreamDesc,
    uint32_t maxExtraDisplayFrameNum,
    Av1AomDecInstanceMemoryDesc* pMemDesc);

extern Av1AomDecReturnType av1AomDecCreate(
    const Av1AomDecInstanceDesc* pInstanceDesc,
    void** ppContext);

extern Av1AomDecReturnType av1AomDecQueryFrameSize(
    uint32_t width,
    uint32_t height,
    uint32_t bitDepth,
    Av1AomDecPictureFormat format,
    uint32_t* pSize);

extern Av1AomDecReturnType av1AomDecQueryInstanceCharacteristics(
    const Av1AomDecStreamDesc* pStreamDesc,
    Av1AomDecInstanceCharacteristics* pChars);

extern Av1AomDecReturnType av1AomDecDecodeAu(
    void* pContext,
    const uint8_t* pData,
    uint32_t size,
    const Av1AomDecAuDecodingControl* pControl);

extern Av1AomDecReturnType av1AomDecSyncAu(
    void* pContext,
    Av1AomDecAuResult* pResult);

extern Av1AomDecReturnType av1AomDecSetDecodeOutput(
    void* pContext,
    void* pBuffer,
    uint32_t size);

extern Av1AomDecReturnType av1AomDecSyncDecodeOutput(
    void* pContext,
    Av1AomDecOutputInfoBuffer* pInfo,
    uint64_t* pPts);

extern Av1AomDecReturnType av1AomDecFlush(void* pContext);

extern Av1AomDecReturnType av1AomDecReset(void* pContext);

extern Av1AomDecReturnType av1AomDecDestroy(void* pContext);

#ifdef __cplusplus
}
#endif

#endif /* _AV1_AOM_DEC_H_ */
```

## 3. Symbol Mapping

| VP9 Custom API (sce_prvp9dec.h) | AV1 Equivalent | Change Type |
|----------------------------------|----------------|-------------|
| `SCE_PRVP9DEC_SUCCESS` | `AV1_AOM_DEC_SUCCESS` | RENAME |
| `SCE_PRVP9DEC_ERR_INVALID_ARGUMENT` | `AV1_AOM_DEC_ERR_INVALID_PARAM` | RENAME (semantic alignment) |
| `SCE_PRVP9DEC_ERR_INVALID_CONTEXT` | `AV1_AOM_DEC_ERR_INVALID_CTX` | RENAME |
| `SCE_PRVP9DEC_ERR_MEMORY_ALLOC` | `AV1_AOM_DEC_ERR_MEM_ALLOC` | RENAME |
| `SCE_PRVP9DEC_ERR_UNSUPPORTED_PROFILE` | `AV1_AOM_DEC_ERR_UNSUP_BITSTREAM` | RENAME |
| `SCE_PRVP9DEC_ERR_BITSTREAM` | `AV1_AOM_DEC_ERR_UNSUP_BITSTREAM` | RENAME |
| `SCE_PRVP9DEC_ERR_FRAME_DECODE` | `AV1_AOM_DEC_ERR_CORRUPT_FRAME` | RENAME |
| `SCE_PRVP9DEC_ERR_QUEUE_FULL` | *REMOVE* | REMOVE (no queueing in AOM) |
| `SCE_PRVP9DEC_ERR_QUEUE_EMPTY` | *REMOVE* | REMOVE (handled via NULL return) |
| `SCE_PRVP9DEC_ERR_TIMEOUT` | *REMOVED* | REMOVE (not supported by AOM) |
| `SCE_PRVP9DEC_ERR_NOT_ENOUGH_BUFFER` | `AV1_AOM_DEC_ERR_BUF_TOO_SMALL` | RENAME |
| `ScePrVp9DecContext` | `Av1AomDecContext` (via `void*`) | RENAME |
| `scePrVp9DecCreate()` | `av1AomDecCreate()` | RENAME |
| `numFrames` field | *REMOVED* from input/output structs | MODIFY |

> Full mapping table provided in prior step.

## 4. Implementation Files

| File | AOM Functions Used | VP9 → AV1 Logic Changes |
|------|-------------------|-------------------------|
| `av1api_dec_create.cpp` | `aom_codec_dec_init_ver()`, `aom_codec_version()` | Removes profile detection via peek stream info — AV1 uses OBU parsing in `peek_si`. Drops GPU affinity/thread priority setup. |
| `av1api_query_dec_memory.cpp` | `aom_codec_get_mem_usage()` (probe), `aom_codec_destroy()` | No GPU-specific memory hints; uses generic AOM memory query. |
| `av1api_decode_au.cpp` | `aom_codec_decode()`, `aom_codec_get_frame()` (optional flush) | Prepends Annex-B start code if `usesAnnexB=true`. Drops raw YUV input assumptions — AV1 expects OBUs/IVF/Opaque container. |
| `av1api_sync_au.cpp` | `aom_codec_get_frame()` (repeated until NULL) | Maps to AOM’s iterator pattern; returns empty result via NULL, not error code. |
| `av1api_sync_output.cpp` | None (pure data copy) | Converts `YV12_BUFFER_CONFIG` → planar image using AV1 subsampling (`y=1`, `x=1`). |
| `av1api_set_output.cpp` | `aom_codec_control(ctx, AV1_SET_FRAME_BUFFER_SIZE_CB)` | Registers external frame buffer callbacks; drops GPU sync event registration. |
| `av1api_flush.cpp` | *REMOVED* | No flush API in AOM — caller must call `get_frame()` until NULL. |
| `av1api_reset.cpp` | `aom_codec_destroy()`, `aom_codec_dec_init_ver()` (reinit) | Drops VP9 reference frame reset logic — handled internally by reinit. |
| `av1api_destroy.cpp` | `aom_codec_destroy()` | Cleans up context and external buffers if used. |
| `av1api_query_characteristics.cpp` | `aom_codec_peek_stream_info()`, `aom_codec_get_stream_info()` | Extracts AV1 profile (from sequence header), level, dimensions. |
| `av1api_query_frame_size.cpp` | None (arithmetic only) | Adds 10-bit support and correct chroma stride calculation for YUV420/NV12. |
| `av1api_version.cpp` | `aom_codec_version()`, `AOM_CODEC_VER` macro | Returns AOM version as `0xMMNNPP`. |
| `av1_internals.h/cpp` | None (helpers only) | Implements `Av1AomDecoder` struct, error mapping, Annex-B prepending, frame buffer registration. |

## 5. Internal Helpers (`av1_internals.h/cpp`)

### Decoder wrapper struct

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
    
    void* pending_output_buffer;      // user-provided buffer for current frame
    uint32_t pending_output_size;
    int has_pending_frame;            // true if get_frame() returned a frame but not yet consumed
    
    aom_get_frame_buffer_cb_fn_t fb_get_cb;
    aom_release_frame_buffer_cb_fn_t fb_release_cb;
    void* cb_priv;
    
    uint32_t frame_count;             // decoded frames since last reset
    int uses_annex_b;                 // if true, prepend start code before decode
} Av1AomDecoder;
```

### Helper functions

| Function | Purpose |
|----------|---------|
| `av1AomDecMapError()` | Maps AOM error codes to AV1 API codes |
| `av1AomDecExtractStreamInfo()` | Populates stream descriptor from AOM context |
| `av1AomDecPrepareInputBuffer()` | Prepends Annex-B start code if enabled |
| `av1AomDecSetFrameBuffer()` | Registers external output buffer via control interface |
| `av1AomDecGetOutputImage()` | Wraps `aom_codec_get_frame()` with state validation |

## 6. Removed Elements

| Element | Reason for Removal |
|---------|--------------------|
| `GPU_SYNC_OVERRIDE`, `GPU_SYNC_EVENT`, `GPU_INTERRUPT` macros | No GPU sync semantics in open-source AOM decoder |
| `SceKernelCpumask`, `SceUnifiedComputeContextType`, `ScePrVp9DecComputeContextType` | No kernel/GPU thread affinity or unified compute context in AV1 reference decoder |
| `numFrames` field (superframe support) | AV1 has no superframes — each AU is one frame |
| `pSoftwareDecoderContext` (VPX context pointer) | Replaced by direct `aom_codec_ctx_t` storage |
| `recommendedSwSchedulePriority`, `recommendedCopyThreadPriority` | Priority control not exposed in AOM API |
| `scePrVp9DecFlush()` function | No flush API in AOM — caller must drain via repeated `get_frame()` |

## 7. AV1-Specific Additions

| Feature | Description |
|---------|-------------|
| `AV1_AOM_DEC_ERR_OBU_ERROR` (`-25`) | Returned when OBU parsing fails (e.g., malformed header, unsupported OBUs) |
| `AV1_AOM_DEC_ERR_TILE_ERROR` (`-26`) | Returned when tile decoding fails due to bitstream corruption or internal error |
| `usesAnnexB` flag in `Av1AomDecStreamDesc`, `Av1AomDecOutputInfoBuffer` | When set, prepends 4-byte start code (`0x00 0x00 0x00 0x01`) before decoding |
| `numThreads` field in `Av1AomDecInstanceDesc` | Controls tile-level parallelism via AOM’s worker interface (max = tile count) |
| AV1 profile/level enums (`AV1_AOM_DEC_PROFILE_MAIN/HIGH/PROFESSIONAL`, levels 2.0–7.3) | Matches AV1 specification v1.0.0 |

## 8. Build Integration

### Required libaom headers
```c
#include <aom/aom_decoder.h>
#include <aom/aom_codec.h>
#include <aom/aomdx.h>
#include <aom/aom_image.h>
#include <aom/aom_frame_buffer.h>
```

### Suggested CMakeLists.txt snippet
```cmake
# AV1 AOM decoder API layer
add_library(av1_aom_dec STATIC
  av1api_dec_create.cpp
  av1api_query_dec_memory.cpp
  av1api_decode_au.cpp
  av1api_sync_au.cpp
  av1api_set_output.cpp
  av1api_sync_output.cpp
  av1api_reset.cpp
  av1api_destroy.cpp
  av1api_query_characteristics.cpp
  av1api_query_frame_size.cpp
  av1api_version.cpp
  av1_internals.cpp
)

target_include_directories(av1_aom_dec PUBLIC ${AOM_INCLUDE_DIRS})
target_link_libraries(av1_aom_dec PRIVATE aom)
target_compile_definitions(av1_aom_dec PRIVATE AV1_AOM_DEC_EXPORTS)
```

## 9. Testing Strategy

### Unit tests per API function
| Function | Test Cases |
|---------|-----------|
| `av1AomDecGetVersion()` | Returns expected version; no crash on NULL output |
| `av1AomDecQueryMemory()` | Correct size for various resolutions (e.g., 4K, 8K) and thread counts |
| `av1AomDecCreate()` | Success with valid config; error codes for invalid params |
| `av1AomDecDecodeAu()` | Decodes valid AV1 OBUs; returns OBU/TILE errors on corrupted input |
| `av1AomDecSyncAu()` | Returns frames in order; handles empty queue (NULL) gracefully |
| `av1AomDecSetDecodeOutput()` | Registers external buffer; fails with invalid size |
| `av1AomDecReset()` | Clears state; subsequent decode works correctly |
| `av1AomDecDestroy()` | No memory leaks; context pointer invalidated |

### Conformance test approach
- Use official **AV1 Test Vector Set** (aomedia.org/test-vectors)
- Validate:
  - Profile/level compatibility (Main, High, Professional profiles)
  - Bit depth support (8-bit, 10-bit, 12-bit)
  - Chroma subsampling (4:2:0 only in this layer; others can be added later)
  - Annex B vs IVF container handling
- Automated regression:
  - Decode each test vector → compare output PSNR against reference (`aomdec`)
  - Verify frame timestamps match input metadata

> **Note**: All tests should run on CPU-only platforms (no GPU dependencies assumed).
```