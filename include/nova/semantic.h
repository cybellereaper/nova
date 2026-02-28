#pragma once

#include "nova/ast.h"
#include "nova/diagnostic.h"

typedef size_t NovaTypeId;

typedef enum {
    NOVA_TYPE_KIND_UNKNOWN,
    NOVA_TYPE_KIND_NUMBER,
    NOVA_TYPE_KIND_STRING,
    NOVA_TYPE_KIND_BOOL,
    NOVA_TYPE_KIND_UNIT,
    NOVA_TYPE_KIND_LIST,
    NOVA_TYPE_KIND_FUNCTION,
    NOVA_TYPE_KIND_CUSTOM,
} NovaTypeKind;

typedef enum {
    NOVA_EFFECT_NONE = 0,
    NOVA_EFFECT_ASYNC = 1 << 0,
    NOVA_EFFECT_IMPURE = 1 << 1,
} NovaEffectMask;

struct NovaTypeRecord;

typedef struct {
    NovaTypeKind kind;
    union {
        struct {
            NovaTypeId element;
        } list;
        struct {
            NovaTypeId *params;
            size_t param_count;
            NovaTypeId result;
            NovaEffectMask effects;
        } function;
        struct {
            const struct NovaTypeRecord *record;
        } custom;
    } as;
} NovaTypeInfo;

typedef struct NovaScopeEntry {
    NovaToken name;
    NovaTypeId type;
    NovaEffectMask effects;
    bool is_constructor;
    const struct NovaTypeRecord *type_record;
    const NovaVariantDecl *variant_decl;
    struct NovaScopeEntry *next;
} NovaScopeEntry;

typedef struct NovaScope {
    NovaScopeEntry *entries;
    struct NovaScope *parent;
} NovaScope;

typedef struct {
    const NovaVariantDecl *variant;
    size_t arity;
} NovaVariantRecord;

typedef struct NovaTypeRecord {
    const NovaTypeDecl *decl;
    NovaTypeId type_id;
    NovaVariantRecord *variants;
    size_t variant_count;
} NovaTypeRecord;

typedef struct {
    NovaTypeRecord *items;
    size_t count;
    size_t capacity;
} NovaTypeRecordList;

typedef struct {
    const NovaExpr *expr;
    NovaTypeId type;
    NovaEffectMask effects;
} NovaExprInfo;

typedef struct {
    NovaExprInfo *items;
    size_t count;
    size_t capacity;
} NovaExprInfoList;

typedef struct {
    NovaScope *scope;
    NovaDiagnosticList diagnostics;
    NovaTypeInfo *types;
    size_t type_count;
    size_t type_capacity;
    NovaTypeRecordList type_records;
    NovaExprInfoList expr_info;
    const NovaExpr **expr_index_keys;
    size_t *expr_index_values;
    size_t expr_index_count;
    size_t expr_index_capacity;
    NovaTypeId type_unknown;
    NovaTypeId type_unit;
    NovaTypeId type_number;
    NovaTypeId type_string;
    NovaTypeId type_bool;
} NovaSemanticContext;

void nova_semantic_context_init(NovaSemanticContext *ctx);
void nova_semantic_context_free(NovaSemanticContext *ctx);
void nova_semantic_analyze_program(NovaSemanticContext *ctx, const NovaProgram *program);
const NovaExprInfo *nova_semantic_lookup_expr(const NovaSemanticContext *ctx, const NovaExpr *expr);
const NovaTypeInfo *nova_semantic_type_info(const NovaSemanticContext *ctx, NovaTypeId type_id);
const NovaTypeRecord *nova_semantic_find_type(const NovaSemanticContext *ctx, const NovaToken *name);
