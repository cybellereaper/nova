#include "nova/diagnostic.h"

#include <stdlib.h>

void nova_diagnostic_list_init(NovaDiagnosticList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_diagnostic_list_push(NovaDiagnosticList *list, NovaDiagnostic diagnostic) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        NovaDiagnostic *new_items = realloc(list->items, new_capacity * sizeof(NovaDiagnostic));
        if (!new_items) {
            return;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = diagnostic;
}

void nova_diagnostic_list_free(NovaDiagnosticList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

