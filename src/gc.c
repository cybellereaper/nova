#include "nova/gc.h"

#include <stdlib.h>
#include <string.h>

typedef struct NovaGCObjectHeader {
    struct NovaGCObjectHeader *next;
    size_t size;
    NovaGCTraceFn trace;
    void *trace_ctx;
    bool marked;
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

void nova_gc_init(NovaGC *gc, const NovaGCAllocator *allocator) {
    if (!gc) {
        return;
    }
    memset(gc, 0, sizeof(*gc));
    gc->allocator.malloc_fn = default_malloc;
    gc->allocator.free_fn = default_free;
    gc->allocator.user_data = NULL;
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
    header->marked = false;
    gc->objects = header;

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
    if (!header || header->marked) {
        return;
    }
    header->marked = true;
    if (header->trace) {
        header->trace(gc, object, header->trace_ctx);
    }
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

    for (NovaGCObjectHeader *obj = gc->objects; obj; obj = obj->next) {
        obj->marked = false;
    }

    for (NovaGCRoot *root = gc->roots; root; root = root->next) {
        if (root->slot) {
            nova_gc_mark(gc, *root->slot);
        }
    }

    size_t budget = options ? options->sweep_budget : 0;
    bool limited = budget > 0;
    size_t swept = 0;

    NovaGCObjectHeader *prev = NULL;
    NovaGCObjectHeader *obj = gc->objects;
    while (obj) {
        NovaGCObjectHeader *next = obj->next;
        if (!obj->marked) {
            if (prev) {
                prev->next = next;
            } else {
                gc->objects = next;
            }
            gc->allocated_bytes -= obj->size;
            gc->object_count--;
            gc->allocator.free_fn(obj, gc->allocator.user_data);
            swept++;
            if (limited && swept >= budget) {
                break;
            }
        } else {
            prev = obj;
        }
        obj = next;
    }
    gc->collections++;
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
    gc->allocated_bytes = 0;
    gc->object_count = 0;
}
