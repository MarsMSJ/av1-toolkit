```markdown
### Final Review of AV1 API Layer Plan

#### 1. Prohibited References Check

✅ **PASS** — No prohibited references found in the plan document.

- `GPU` appears only in *removed elements* section with explanation ("no GPU decode in open-source AOM") — acceptable.
- `superframe`, `numFrames` appear only as "removed" or "dropped" items — correctly flagged for removal, not used in new API.

#### 2. Technical Correctness

| Area | Assessment | Notes |
|------|------------|-------|
| **AOM function mappings** | ✅ Correct | All core functions (`init`, `decode`, `get_frame`, `destroy`) correctly mapped; control interface usage for external frame buffers is accurate. |
| **AV1 profiles/levels** | ✅ Correct | Profiles (Main/High/Professional) and levels (2.0–7.3) match AV1 spec v1.0.5. Level enum values align with `aom_codec_stream_info_t` usage in libaom. |
| **Error codes** | ⚠️ Minor issue | `AV1_AOM_DEC_ERR_TIMEOUT = -7` retained but marked unused — acceptable for ABI compatibility, but better to remove entirely (see fix below). |
| **Struct field types** | ✅ Correct | All fields use portable types (`uint32_t`, `int`, `void*`). Chroma shift logic implied via subsampling in frame size query is accurate. |

##### Fix: Remove unused timeout error code
- **Current**: `AV1_AOM_DEC_ERR_TIMEOUT = -7`  
  **Issue**: AOM has no timeout semantics; keeping it risks misuse and breaks ABI stability if future versions change error numbering.
- **Fix**:
  ```c
  enum _Av1AomDecReturnType {
      AV1_AOM_DEC_SUCCESS = 0,
      AV1_AOM_DEC_ERR_INVALID_PARAM = -1,
      AV1_AOM_DEC_ERR_INVALID_CTX = -2,
      AV1_AOM_DEC_ERR_MEM_ALLOC = -3,
      AV1_AOM_DEC_ERR_UNSUP_BITSTREAM = -4,
      AV1_AOM_DEC_ERR_UNSUP_FEATURE = -5,
      AV1_AOM_DEC_ERR_CORRUPT_FRAME = -6,
      // Removed: AV1_AOM_DEC_ERR_TIMEOUT
      AV1_AOM_DEC_ERR_BUF_TOO_SMALL = -8,
      AV1_AOM_DEC_ERR_OBU_ERROR = -25,
      AV1_AOM_DEC_ERR_TILE_ERROR = -26,
  };
  ```
- **Rationale**: AOM uses `AOM_CODEC_OK` + NULL return for "no frame ready", not error codes. Timeout is application-layer concern.

#### 3. Completeness

| Item | Status |
|------|--------|
| Every VP9 API function accounted for? | ✅ Yes — all 12 functions mapped; `Flush` explicitly noted as removed (caller responsibility). |
| AV1-specific additions present? | ✅ Yes — OBU error, tile error, Annex B flag, thread count, profile/level enums. |
| Build integration usable? | ⚠️ Minor enhancement needed | CMake snippet lacks `target_compile_definitions` for header guard (`AV1_AOM_DEC_EXPORTS`) — should be conditional on shared/static build. |

##### Fix: Improve CMake snippet
- **Current**:
  ```cmake
  target_compile_definitions(av1_aom_dec PRIVATE AV1_AOM_DEC_EXPORTS)
  ```
- **Fix**:
  ```cmake
  if(BUILD_SHARED_LIBS)
    target_compile_definitions(av1_aom_dec PUBLIC AV1_AOM_DEC_EXPORTS)
  endif()
  ```
- **Rationale**: Ensures proper symbol visibility for shared library builds; static builds don’t need export definitions.

#### 4. Formatting

✅ **PASS** — Markdown is well-structured, consistent, and readable:
- Clear section hierarchy
- Tables used appropriately
- Code blocks properly fenced
- No stray characters or formatting artifacts

---

### Final Verdict

**FINAL REVIEW PASS — Plan is ready for implementation**, with the following *mandatory* fixes applied before coding:

1. **Remove `AV1_AOM_DEC_ERR_TIMEOUT`** from error enum.
2. **Update CMake snippet** to conditionally define export macros.

All other elements are technically sound and complete.
```