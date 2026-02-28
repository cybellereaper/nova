#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NovaGC NovaGC;
typedef struct NovaGCObject NovaGCObject;

typedef void *(*NovaGCAllocFn)(void *ctx, size_t size);
typedef void (*NovaGCFreeFn)(void *ctx, void *ptr);
typedef void (*NovaGCTraceFn)(NovaGC *gc, void *payload);
typedef void (*NovaGCFinalizerFn)(void *payload);

typedef struct {
    NovaGCAllocFn alloc;
    NovaGCFreeFn free;
    void *ctx;
    size_t initial_threshold_bytes;
    size_t growth_percent;
} NovaGCConfig;

typedef struct {
    size_t bytes_allocated;
    size_t bytes_live;
    size_t collections;
    size_t objects_total;
    size_t objects_marked;
    size_t objects_swept;
    size_t root_count;
    bool collection_in_progress;
} NovaGCStats;

NovaGC *nova_gc_create(const NovaGCConfig *config);
void nova_gc_destroy(NovaGC *gc);

void *nova_gc_alloc(NovaGC *gc,
                    size_t payload_size,
                    NovaGCTraceFn trace,
                    NovaGCFinalizerFn finalizer);

bool nova_gc_add_root(NovaGC *gc, void **slot);
void nova_gc_remove_root(NovaGC *gc, void **slot);

void nova_gc_mark_ptr(NovaGC *gc, void *payload);
void nova_gc_collect(NovaGC *gc);
void nova_gc_collect_step(NovaGC *gc, size_t budget_objects);

NovaGCStats nova_gc_stats(const NovaGC *gc);

