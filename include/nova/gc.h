#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NovaGC NovaGC;
typedef struct NovaGCObjectHeader NovaGCObjectHeader;
typedef struct NovaGCRoot NovaGCRoot;

typedef void (*NovaGCTraceFn)(NovaGC *gc, void *object, void *ctx);

typedef struct {
    void *(*malloc_fn)(size_t size, void *user_data);
    void (*free_fn)(void *ptr, void *user_data);
    void *user_data;
} NovaGCAllocator;

typedef struct {
    size_t sweep_budget;
} NovaGCCollectOptions;

typedef struct {
    size_t object_count;
    size_t allocated_bytes;
    size_t peak_allocated_bytes;
    size_t collections;
} NovaGCStats;

struct NovaGC {
    NovaGCObjectHeader *objects;
    NovaGCRoot *roots;
    NovaGCAllocator allocator;
    size_t allocated_bytes;
    size_t peak_allocated_bytes;
    size_t object_count;
    size_t collections;
    uint32_t mark_epoch;
    bool sweep_in_progress;
    NovaGCObjectHeader *sweep_prev;
    NovaGCObjectHeader *sweep_cursor;
    NovaGCObjectHeader **mark_stack;
    size_t mark_stack_count;
    size_t mark_stack_capacity;
    bool mark_draining;
};

void nova_gc_init(NovaGC *gc, const NovaGCAllocator *allocator);
void nova_gc_destroy(NovaGC *gc);

void *nova_gc_alloc(NovaGC *gc, size_t size, NovaGCTraceFn trace, void *trace_ctx);
void nova_gc_mark(NovaGC *gc, void *object);

void nova_gc_add_root(NovaGC *gc, void **root_slot);
void nova_gc_remove_root(NovaGC *gc, void **root_slot);

size_t nova_gc_collect(NovaGC *gc, const NovaGCCollectOptions *options);
void nova_gc_stats(const NovaGC *gc, NovaGCStats *out_stats);
