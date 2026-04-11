#ifndef AV1_MEM_OVERRIDE_H
#define AV1_MEM_OVERRIDE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Av1StreamInfo {
    int width;
    int height;
    int max_bitrate;
    int chroma_subsampling;
    bool is_16bit;
} Av1StreamInfo;

typedef struct Av1MemStats {
    size_t total_size;
    size_t used_size;
    size_t peak_usage;
    size_t num_allocations;
    size_t num_frees;
    size_t num_free_list_hits;
    size_t num_bump_allocations;
    size_t largest_free_block;
} Av1MemStats;

bool av1_mem_init(void *base, size_t size);
void av1_mem_shutdown(void);
void *av1_mem_malloc(size_t size);
void *av1_mem_memalign(size_t alignment, size_t size);
void *av1_mem_calloc(size_t num, size_t size);
void av1_mem_free(void *ptr);
Av1MemStats av1_mem_get_stats(void);
void av1_mem_reset_stats(void);
size_t av1_mem_query_size(const Av1StreamInfo *info, int queue_depth, int num_workers);
bool av1_mem_is_initialized(void);
void *av1_mem_get_base(void);
size_t av1_mem_get_total_size(void);

void *av1_mem_override_malloc(size_t size);
void *av1_mem_override_memalign(size_t alignment, size_t size);
void *av1_mem_override_calloc(size_t num, size_t size);
void av1_mem_override_free(void *ptr);
void av1_mem_set_override_enabled(bool enabled);
bool av1_mem_get_override_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* AV1_MEM_OVERRIDE_H */
