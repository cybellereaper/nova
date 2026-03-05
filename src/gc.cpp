#include "nova/gc.h"

#include <stdlib.h>
#include <string.h>

typedef struct NovaGCObject {
    struct NovaGCObject *next;
    size_t payload_size;
    NovaGCTraceFn trace;
    NovaGCFinalizerFn finalizer;
    uint8_t marked;
    uint8_t _padding[7];
} NovaGCObject;

struct NovaGC {
    NovaGCAllocFn alloc;
    NovaGCFreeFn free;
    void *alloc_ctx;

    NovaGCObject *objects;
    size_t object_count;
    size_t bytes_allocated;
    size_t bytes_live;

    void ***roots;
    size_t root_count;
    size_t root_capacity;

    NovaGCObject **mark_stack;
    size_t mark_count;
    size_t mark_capacity;

    size_t collections;
    size_t objects_marked;
    size_t objects_swept;

    size_t threshold_bytes;
    size_t growth_percent;

    bool collect_in_progress;
    bool roots_scanned;
    NovaGCObject **sweep_cursor;
};

static void *default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void default_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static bool ensure_root_capacity(NovaGC *gc, size_t needed) {
    if (needed <= gc->root_capacity) {
        return true;
    }
    size_t next_capacity = gc->root_capacity == 0 ? 16 : gc->root_capacity * 2;
    while (next_capacity < needed) {
        next_capacity *= 2;
    }
    void ***next = static_cast<void ***>(gc->alloc(gc->alloc_ctx, next_capacity * sizeof(void **)));
    if (!next) {
        return false;
    }
    if (gc->roots) {
        memcpy(next, gc->roots, gc->root_count * sizeof(void **));
        gc->free(gc->alloc_ctx, gc->roots);
    }
    gc->roots = next;
    gc->root_capacity = next_capacity;
    return true;
}

static bool ensure_mark_capacity(NovaGC *gc, size_t needed) {
    if (needed <= gc->mark_capacity) {
        return true;
    }
    size_t next_capacity = gc->mark_capacity == 0 ? 64 : gc->mark_capacity * 2;
    while (next_capacity < needed) {
        next_capacity *= 2;
    }
    NovaGCObject **next = static_cast<NovaGCObject **>(gc->alloc(gc->alloc_ctx, next_capacity * sizeof(NovaGCObject *)));
    if (!next) {
        return false;
    }
    if (gc->mark_stack) {
        memcpy(next, gc->mark_stack, gc->mark_count * sizeof(NovaGCObject *));
        gc->free(gc->alloc_ctx, gc->mark_stack);
    }
    gc->mark_stack = next;
    gc->mark_capacity = next_capacity;
    return true;
}

static NovaGCObject *header_from_payload(void *payload) {
    if (!payload) {
        return NULL;
    }
    return ((NovaGCObject *)payload) - 1;
}

static void begin_collection(NovaGC *gc) {
    gc->collect_in_progress = true;
    gc->roots_scanned = false;
    gc->sweep_cursor = &gc->objects;
    gc->objects_marked = 0;
    gc->objects_swept = 0;
    gc->collections += 1;
}

static void scan_roots(NovaGC *gc) {
    if (gc->roots_scanned) {
        return;
    }
    for (size_t i = 0; i < gc->root_count; ++i) {
        void **slot = gc->roots[i];
        if (slot) {
            nova_gc_mark_ptr(gc, *slot);
        }
    }
    gc->roots_scanned = true;
}

static void trace_some(NovaGC *gc, size_t budget) {
    size_t traced = 0;
    while (gc->mark_count > 0 && traced < budget) {
        NovaGCObject *object = gc->mark_stack[--gc->mark_count];
        if (object->trace) {
            object->trace(gc, (void *)(object + 1));
        }
        traced += 1;
    }
}

static void sweep_some(NovaGC *gc, size_t budget) {
    size_t swept = 0;
    while (*gc->sweep_cursor && swept < budget) {
        NovaGCObject *object = *gc->sweep_cursor;
        if (object->marked) {
            object->marked = 0;
            gc->bytes_live += object->payload_size;
            gc->sweep_cursor = &object->next;
            swept += 1;
            continue;
        }

        *gc->sweep_cursor = object->next;
        if (object->finalizer) {
            object->finalizer((void *)(object + 1));
        }
        gc->bytes_allocated -= object->payload_size;
        gc->object_count -= 1;
        gc->objects_swept += 1;
        gc->free(gc->alloc_ctx, object);
        swept += 1;
    }
}

NovaGC *nova_gc_create(const NovaGCConfig *config) {
    NovaGCAllocFn alloc = default_alloc;
    NovaGCFreeFn free_fn = default_free;
    void *ctx = NULL;
    size_t threshold = 64 * 1024;
    size_t growth = 150;

    if (config) {
        if (config->alloc) {
            alloc = config->alloc;
        }
        if (config->free) {
            free_fn = config->free;
        }
        ctx = config->ctx;
        if (config->initial_threshold_bytes > 0) {
            threshold = config->initial_threshold_bytes;
        }
        if (config->growth_percent > 0) {
            growth = config->growth_percent;
        }
    }

    NovaGC *gc = static_cast<NovaGC *>(alloc(ctx, sizeof(NovaGC)));
    if (!gc) {
        return NULL;
    }
    memset(gc, 0, sizeof(*gc));
    gc->alloc = alloc;
    gc->free = free_fn;
    gc->alloc_ctx = ctx;
    gc->threshold_bytes = threshold;
    gc->growth_percent = growth;
    return gc;
}

void nova_gc_destroy(NovaGC *gc) {
    if (!gc) {
        return;
    }

    NovaGCObject *current = gc->objects;
    while (current) {
        NovaGCObject *next = current->next;
        if (current->finalizer) {
            current->finalizer((void *)(current + 1));
        }
        gc->free(gc->alloc_ctx, current);
        current = next;
    }

    gc->free(gc->alloc_ctx, gc->roots);
    gc->free(gc->alloc_ctx, gc->mark_stack);
    gc->free(gc->alloc_ctx, gc);
}

void *nova_gc_alloc(NovaGC *gc,
                    size_t payload_size,
                    NovaGCTraceFn trace,
                    NovaGCFinalizerFn finalizer) {
    if (!gc || payload_size == 0) {
        return NULL;
    }

    size_t total = sizeof(NovaGCObject) + payload_size;
    NovaGCObject *object = static_cast<NovaGCObject *>(gc->alloc(gc->alloc_ctx, total));
    if (!object) {
        return NULL;
    }

    memset(object, 0, total);
    object->payload_size = payload_size;
    object->trace = trace;
    object->finalizer = finalizer;
    object->next = gc->objects;
    gc->objects = object;
    gc->object_count += 1;
    gc->bytes_allocated += payload_size;

    if (gc->bytes_allocated > gc->threshold_bytes) {
        nova_gc_collect_step(gc, 128);
    }

    return (void *)(object + 1);
}

bool nova_gc_add_root(NovaGC *gc, void **slot) {
    if (!gc || !slot) {
        return false;
    }
    if (!ensure_root_capacity(gc, gc->root_count + 1)) {
        return false;
    }
    gc->roots[gc->root_count++] = slot;
    return true;
}

void nova_gc_remove_root(NovaGC *gc, void **slot) {
    if (!gc || !slot) {
        return;
    }
    for (size_t i = 0; i < gc->root_count; ++i) {
        if (gc->roots[i] == slot) {
            gc->roots[i] = gc->roots[gc->root_count - 1];
            gc->root_count -= 1;
            return;
        }
    }
}

void nova_gc_mark_ptr(NovaGC *gc, void *payload) {
    if (!gc || !payload) {
        return;
    }

    NovaGCObject *object = header_from_payload(payload);
    if (object->marked) {
        return;
    }
    object->marked = 1;
    gc->objects_marked += 1;

    if (!ensure_mark_capacity(gc, gc->mark_count + 1)) {
        return;
    }
    gc->mark_stack[gc->mark_count++] = object;
}

void nova_gc_collect_step(NovaGC *gc, size_t budget_objects) {
    if (!gc) {
        return;
    }

    if (budget_objects == 0) {
        budget_objects = 1;
    }

    if (!gc->collect_in_progress) {
        begin_collection(gc);
    }

    if (!gc->roots_scanned) {
        gc->bytes_live = 0;
        scan_roots(gc);
    }

    size_t trace_budget = budget_objects / 2;
    size_t sweep_budget = budget_objects - trace_budget;
    if (trace_budget == 0) {
        trace_budget = 1;
    }
    if (sweep_budget == 0) {
        sweep_budget = 1;
    }

    trace_some(gc, trace_budget);
    if (gc->mark_count == 0) {
        sweep_some(gc, sweep_budget);
    }

    if (gc->mark_count == 0 && *gc->sweep_cursor == NULL) {
        gc->collect_in_progress = false;
        size_t grown = gc->bytes_live + (gc->bytes_live * gc->growth_percent) / 100;
        if (grown < 1024) {
            grown = 1024;
        }
        gc->threshold_bytes = grown;
    }
}

void nova_gc_collect(NovaGC *gc) {
    if (!gc) {
        return;
    }

    do {
        nova_gc_collect_step(gc, 256);
    } while (gc->collect_in_progress);
}

NovaGCStats nova_gc_stats(const NovaGC *gc) {
    NovaGCStats stats;
    memset(&stats, 0, sizeof(stats));
    if (!gc) {
        return stats;
    }
    stats.bytes_allocated = gc->bytes_allocated;
    stats.bytes_live = gc->bytes_live;
    stats.collections = gc->collections;
    stats.objects_total = gc->object_count;
    stats.objects_marked = gc->objects_marked;
    stats.objects_swept = gc->objects_swept;
    stats.root_count = gc->root_count;
    stats.collection_in_progress = gc->collect_in_progress;
    return stats;
}
