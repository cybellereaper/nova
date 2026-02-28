#include "nova/gc.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct NovaGCObjectHeader {
    struct NovaGCObjectHeader *next;
    size_t size;
    NovaGCTraceFn trace;
    void *trace_ctx;
    uint32_t mark_epoch;
} NovaGCObjectHeader;

typedef struct NovaGCRoot {
    void **slot;
    struct NovaGCRoot *next;
} NovaGCRoot;

static void *default_malloc(size_t size, void *user_data) {
    (void)user_data;
    return malloc(size);
}

static void default_free(void *ptr, void *user_data) {
    (void)user_data;
    free(ptr);
}

static NovaGCObjectHeader *nova_gc_header_from_object(void *object) {
    if (!object) {
        return NULL;
    }
    return ((NovaGCObjectHeader *)object) - 1;
}

static bool mark_stack_push(NovaGC *gc, NovaGCObjectHeader *header) {
    if (gc->mark_stack_count == gc->mark_stack_capacity) {
        size_t new_capacity = gc->mark_stack_capacity == 0 ? 32 : gc->mark_stack_capacity * 2;
        size_t bytes = new_capacity * sizeof(NovaGCObjectHeader *);
        NovaGCObjectHeader **new_stack = (NovaGCObjectHeader **)gc->allocator.malloc_fn(bytes, gc->allocator.user_data);
        if (!new_stack) {
            return false;
        }
        if (gc->mark_stack_count > 0) {
            memcpy(new_stack, gc->mark_stack, gc->mark_stack_count * sizeof(NovaGCObjectHeader *));
        }
        if (gc->mark_stack) {
            gc->allocator.free_fn(gc->mark_stack, gc->allocator.user_data);
        }
        gc->mark_stack = new_stack;
        gc->mark_stack_capacity = new_capacity;
    }
    gc->mark_stack[gc->mark_stack_count++] = header;
    return true;
}

static uint32_t next_mark_epoch(NovaGC *gc) {
    if (gc->mark_epoch == UINT32_MAX) {
        gc->mark_epoch = 1;
        for (NovaGCObjectHeader *obj = gc->objects; obj; obj = obj->next) {
            obj->mark_epoch = 0;
        }
        return gc->mark_epoch;
    }
    gc->mark_epoch += 1;
    if (gc->mark_epoch == 0) {
        gc->mark_epoch = 1;
    }
    return gc->mark_epoch;
}

static void begin_collection_cycle(NovaGC *gc) {
    next_mark_epoch(gc);
    for (NovaGCRoot *root = gc->roots; root; root = root->next) {
        if (root->slot && *root->slot) {
            nova_gc_mark(gc, *root->slot);
        }
    }
    gc->sweep_prev = NULL;
    gc->sweep_cursor = gc->objects;
    gc->sweep_in_progress = true;
}

void nova_gc_init(NovaGC *gc, const NovaGCAllocator *allocator) {
    if (!gc) {
        return;
    }
    memset(gc, 0, sizeof(*gc));
    gc->allocator.malloc_fn = default_malloc;
    gc->allocator.free_fn = default_free;
    gc->allocator.user_data = NULL;
    gc->mark_epoch = 1;
    if (allocator) {
        if (allocator->malloc_fn) {
            gc->allocator.malloc_fn = allocator->malloc_fn;
        }
        if (allocator->free_fn) {
            gc->allocator.free_fn = allocator->free_fn;
        }
        gc->allocator.user_data = allocator->user_data;
    }
}

void *nova_gc_alloc(NovaGC *gc, size_t size, NovaGCTraceFn trace, void *trace_ctx) {
    if (!gc || size == 0) {
        return NULL;
    }
    size_t total = sizeof(NovaGCObjectHeader) + size;
    NovaGCObjectHeader *header = (NovaGCObjectHeader *)gc->allocator.malloc_fn(total, gc->allocator.user_data);
    if (!header) {
        return NULL;
    }
    header->next = gc->objects;
    header->size = size;
    header->trace = trace;
    header->trace_ctx = trace_ctx;
    header->mark_epoch = gc->sweep_in_progress ? gc->mark_epoch : 0;
    gc->objects = header;
    if (gc->sweep_in_progress && gc->sweep_prev == NULL && gc->sweep_cursor != NULL) {
        gc->sweep_prev = header;
    }

    gc->object_count++;
    gc->allocated_bytes += size;
    if (gc->allocated_bytes > gc->peak_allocated_bytes) {
        gc->peak_allocated_bytes = gc->allocated_bytes;
    }

    void *payload = (void *)(header + 1);
    memset(payload, 0, size);
    return payload;
}

void nova_gc_mark(NovaGC *gc, void *object) {
    if (!gc || !object) {
        return;
    }
    NovaGCObjectHeader *header = nova_gc_header_from_object(object);
    if (!header || header->mark_epoch == gc->mark_epoch) {
        return;
    }
    header->mark_epoch = gc->mark_epoch;
    if (!mark_stack_push(gc, header)) {
        return;
    }
    if (gc->mark_draining) {
        return;
    }

    gc->mark_draining = true;
    while (gc->mark_stack_count > 0) {
        NovaGCObjectHeader *current = gc->mark_stack[--gc->mark_stack_count];
        if (current->trace) {
            current->trace(gc, (void *)(current + 1), current->trace_ctx);
        }
    }
    gc->mark_draining = false;
}

void nova_gc_add_root(NovaGC *gc, void **root_slot) {
    if (!gc || !root_slot) {
        return;
    }
    NovaGCRoot *root = (NovaGCRoot *)gc->allocator.malloc_fn(sizeof(NovaGCRoot), gc->allocator.user_data);
    if (!root) {
        return;
    }
    root->slot = root_slot;
    root->next = gc->roots;
    gc->roots = root;
}

void nova_gc_remove_root(NovaGC *gc, void **root_slot) {
    if (!gc || !root_slot) {
        return;
    }
    NovaGCRoot *prev = NULL;
    NovaGCRoot *node = gc->roots;
    while (node) {
        if (node->slot == root_slot) {
            if (prev) {
                prev->next = node->next;
            } else {
                gc->roots = node->next;
            }
            gc->allocator.free_fn(node, gc->allocator.user_data);
            return;
        }
        prev = node;
        node = node->next;
    }
}

size_t nova_gc_collect(NovaGC *gc, const NovaGCCollectOptions *options) {
    if (!gc) {
        return 0;
    }

    size_t budget = options ? options->sweep_budget : 0;
    size_t limit = (budget == 0) ? SIZE_MAX : budget;

    if (!gc->sweep_in_progress) {
        begin_collection_cycle(gc);
    }

    size_t swept = 0;
    while (gc->sweep_cursor && swept < limit) {
        NovaGCObjectHeader *obj = gc->sweep_cursor;
        NovaGCObjectHeader *next = obj->next;
        if (obj->mark_epoch != gc->mark_epoch) {
            if (gc->sweep_prev) {
                gc->sweep_prev->next = next;
            } else {
                gc->objects = next;
            }
            gc->allocated_bytes -= obj->size;
            gc->object_count--;
            gc->allocator.free_fn(obj, gc->allocator.user_data);
            swept++;
            gc->sweep_cursor = next;
            continue;
        }
        gc->sweep_prev = obj;
        gc->sweep_cursor = next;
    }

    if (!gc->sweep_cursor) {
        gc->sweep_in_progress = false;
        gc->sweep_prev = NULL;
        gc->collections++;
    }

    return swept;
}

void nova_gc_stats(const NovaGC *gc, NovaGCStats *out_stats) {
    if (!gc || !out_stats) {
        return;
    }
    out_stats->object_count = gc->object_count;
    out_stats->allocated_bytes = gc->allocated_bytes;
    out_stats->peak_allocated_bytes = gc->peak_allocated_bytes;
    out_stats->collections = gc->collections;
}

void nova_gc_destroy(NovaGC *gc) {
    if (!gc) {
        return;
    }
    NovaGCObjectHeader *obj = gc->objects;
    while (obj) {
        NovaGCObjectHeader *next = obj->next;
        gc->allocator.free_fn(obj, gc->allocator.user_data);
        obj = next;
    }
    gc->objects = NULL;

    NovaGCRoot *root = gc->roots;
    while (root) {
        NovaGCRoot *next = root->next;
        gc->allocator.free_fn(root, gc->allocator.user_data);
        root = next;
    }
    gc->roots = NULL;

    if (gc->mark_stack) {
        gc->allocator.free_fn(gc->mark_stack, gc->allocator.user_data);
    }
    gc->mark_stack = NULL;
    gc->mark_stack_count = 0;
    gc->mark_stack_capacity = 0;

    gc->allocated_bytes = 0;
    gc->object_count = 0;
    gc->sweep_in_progress = false;
    gc->sweep_prev = NULL;
    gc->sweep_cursor = NULL;
    gc->mark_draining = false;
}
