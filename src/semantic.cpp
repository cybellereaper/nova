#include "nova/semantic.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool token_equals(const NovaToken *a, const NovaToken *b) {
    if (!a || !b) return false;
    if (a->length != b->length) return false;
    return strncmp(a->lexeme, b->lexeme, a->length) == 0;
}

static bool token_equals_cstr(const NovaToken *token, const char *text) {
    size_t len = strlen(text);
    return token && token->length == len && strncmp(token->lexeme, text, len) == 0;
}

static NovaScope *scope_push(NovaScope *parent) {
    NovaScope *scope = calloc(1, sizeof(NovaScope));
    scope->parent = parent;
    scope->entries = NULL;
    return scope;
}

static void scope_free(NovaScope *scope) {
    NovaScopeEntry *entry = scope->entries;
    while (entry) {
        NovaScopeEntry *next = entry->next;
        free(entry);
        entry = next;
    }
    free(scope);
}

static NovaScopeEntry *scope_lookup(const NovaScope *scope, const NovaToken *name) {
    for (const NovaScope *current = scope; current; current = current->parent) {
        for (NovaScopeEntry *entry = current->entries; entry; entry = entry->next) {
            if (token_equals(&entry->name, name)) {
                return entry;
            }
        }
    }
    return NULL;
}

static void diagnostics_error(NovaSemanticContext *ctx, NovaToken token, const char *message) {
    nova_diagnostic_list_push(&ctx->diagnostics, (NovaDiagnostic){
        .token = token,
        .message = message,
        .severity = NOVA_DIAGNOSTIC_ERROR,
    });
}

static void diagnostics_warning(NovaSemanticContext *ctx, NovaToken token, const char *message) {
    nova_diagnostic_list_push(&ctx->diagnostics, (NovaDiagnostic){
        .token = token,
        .message = message,
        .severity = NOVA_DIAGNOSTIC_WARNING,
    });
}

static void scope_define(NovaSemanticContext *ctx, NovaScope *scope, NovaScopeEntry entry) {
    for (NovaScopeEntry *it = scope->entries; it; it = it->next) {
        if (token_equals(&it->name, &entry.name)) {
            diagnostics_error(ctx, entry.name, "symbol already defined in scope");
            return;
        }
    }
    NovaScopeEntry *allocated = malloc(sizeof(NovaScopeEntry));
    if (!allocated) {
        return;
    }
    *allocated = entry;
    allocated->next = scope->entries;
    scope->entries = allocated;
}

static void expr_info_list_init(NovaExprInfoList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void expr_info_list_free(NovaExprInfoList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void expr_info_list_record(NovaExprInfoList *list, const NovaExpr *expr, NovaTypeId type, NovaEffectMask effects) {
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].expr == expr) {
            list->items[i].type = type;
            list->items[i].effects = effects;
            return;
        }
    }
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        NovaExprInfo *items = realloc(list->items, new_capacity * sizeof(NovaExprInfo));
        if (!items) {
            return;
        }
        list->items = items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = (NovaExprInfo){ .expr = expr, .type = type, .effects = effects };
}

static void type_record_list_init(NovaTypeRecordList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void type_record_list_free(NovaTypeRecordList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].variants);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static NovaTypeRecord *type_record_add(NovaTypeRecordList *list, const NovaTypeDecl *decl) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        NovaTypeRecord *items = realloc(list->items, new_capacity * sizeof(NovaTypeRecord));
        if (!items) {
            return NULL;
        }
        list->items = items;
        list->capacity = new_capacity;
    }
    NovaTypeRecord *record = &list->items[list->count++];
    record->decl = decl;
    record->type_id = 0;
    record->variants = NULL;
    record->variant_count = 0;
    return record;
}

static void type_pool_reserve(NovaSemanticContext *ctx) {
    if (ctx->type_count == ctx->type_capacity) {
        size_t new_capacity = ctx->type_capacity == 0 ? 8 : ctx->type_capacity * 2;
        NovaTypeInfo *items = realloc(ctx->types, new_capacity * sizeof(NovaTypeInfo));
        if (!items) {
            return;
        }
        ctx->types = items;
        ctx->type_capacity = new_capacity;
    }
}

static NovaTypeId type_pool_add(NovaSemanticContext *ctx, NovaTypeInfo info) {
    type_pool_reserve(ctx);
    ctx->types[ctx->type_count] = info;
    return ctx->type_count++;
}

static const NovaTypeRecord *type_record_find(const NovaSemanticContext *ctx, const NovaToken *name) {
    for (size_t i = 0; i < ctx->type_records.count; ++i) {
        if (token_equals(&ctx->type_records.items[i].decl->name, name)) {
            return &ctx->type_records.items[i];
        }
    }
    return NULL;
}

static NovaTypeId resolve_type_token(NovaSemanticContext *ctx, const NovaToken *token) {
    if (!token || token->type == NOVA_TOKEN_ERROR) {
        return ctx->type_unknown;
    }
    if (token_equals_cstr(token, "Number")) return ctx->type_number;
    if (token_equals_cstr(token, "String")) return ctx->type_string;
    if (token_equals_cstr(token, "Bool")) return ctx->type_bool;
    if (token_equals_cstr(token, "Unit")) return ctx->type_unit;
    const NovaTypeRecord *record = type_record_find(ctx, token);
    if (record) {
        return record->type_id;
    }
    diagnostics_error(ctx, *token, "unknown type name");
    return ctx->type_unknown;
}

static NovaTypeId type_list(NovaSemanticContext *ctx, NovaTypeId element) {
    NovaTypeInfo info;
    info.kind = NOVA_TYPE_KIND_LIST;
    info.as.list.element = element;
    return type_pool_add(ctx, info);
}

static NovaTypeId type_function(NovaSemanticContext *ctx, const NovaTypeId *params, size_t param_count, NovaTypeId result, NovaEffectMask effects) {
    NovaTypeInfo info;
    info.kind = NOVA_TYPE_KIND_FUNCTION;
    info.as.function.param_count = param_count;
    info.as.function.result = result;
    info.as.function.effects = effects;
    if (param_count > 0) {
        info.as.function.params = malloc(param_count * sizeof(NovaTypeId));
        if (!info.as.function.params) {
            info.as.function.param_count = 0;
        } else {
            memcpy(info.as.function.params, params, param_count * sizeof(NovaTypeId));
        }
    } else {
        info.as.function.params = NULL;
    }
    return type_pool_add(ctx, info);
}

static NovaTypeId type_custom(NovaSemanticContext *ctx, const NovaTypeRecord *record) {
    NovaTypeInfo info;
    info.kind = NOVA_TYPE_KIND_CUSTOM;
    info.as.custom.record = record;
    return type_pool_add(ctx, info);
}

static NovaTypeId unify_types(NovaSemanticContext *ctx, NovaTypeId a, NovaTypeId b, NovaToken at_token) {
    if (a == ctx->type_unknown) return b;
    if (b == ctx->type_unknown) return a;
    if (a == b) return a;
    diagnostics_error(ctx, at_token, "type mismatch");
    return ctx->type_unknown;
}

static void register_type_decl(NovaSemanticContext *ctx, const NovaTypeDecl *decl) {
    NovaTypeRecord *record = type_record_add(&ctx->type_records, decl);
    if (!record) return;
    record->type_id = type_custom(ctx, record);
    ctx->types[record->type_id].as.custom.record = record;
    if (decl->kind == NOVA_TYPE_DECL_SUM) {
        record->variant_count = decl->variants.count;
        record->variants = calloc(record->variant_count, sizeof(*record->variants));
        for (size_t i = 0; i < decl->variants.count; ++i) {
            const NovaVariantDecl *variant = &decl->variants.items[i];
            record->variants[i].variant = variant;
            record->variants[i].arity = variant->payload.count;
            if (variant->payload.count > 0) {
                NovaTypeId *params = malloc((variant->payload.count) * sizeof(NovaTypeId));
                for (size_t p = 0; p < variant->payload.count; ++p) {
                    NovaTypeId param_type = ctx->type_unknown;
                    if (variant->payload.items[p].has_type) {
                        param_type = resolve_type_token(ctx, &variant->payload.items[p].type_name);
                    }
                    params[p] = param_type;
                }
                NovaTypeId fn_type = type_function(ctx, params, variant->payload.count, record->type_id, NOVA_EFFECT_NONE);
                free(params);
                scope_define(ctx, ctx->scope, (NovaScopeEntry){
                    .name = variant->name,
                    .type = fn_type,
                    .effects = NOVA_EFFECT_NONE,
                    .is_constructor = true,
                    .type_record = record,
                    .variant_decl = variant,
                });
            } else {
                scope_define(ctx, ctx->scope, (NovaScopeEntry){
                    .name = variant->name,
                    .type = record->type_id,
                    .effects = NOVA_EFFECT_NONE,
                    .is_constructor = true,
                    .type_record = record,
                    .variant_decl = variant,
                });
            }
        }
    } else {
        if (decl->tuple_fields.count == 0) {
            diagnostics_warning(ctx, decl->name, "tuple type has no fields");
        } else {
            for (size_t i = 0; i < decl->tuple_fields.count; ++i) {
                if (!decl->tuple_fields.items[i].has_type) {
                    diagnostics_warning(ctx, decl->tuple_fields.items[i].name, "tuple field missing type annotation");
                }
            }
        }
    }
}

static NovaTypeId analyze_expr(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects);

static NovaTypeId analyze_block(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaScope *inner = scope_push(scope);
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId type = ctx->type_unit;
    for (size_t i = 0; i < expr->as.block.expressions.count; ++i) {
        NovaEffectMask expr_effects = NOVA_EFFECT_NONE;
        type = analyze_expr(ctx, inner, expr->as.block.expressions.items[i], &expr_effects);
        effects |= expr_effects;
    }
    scope_free(inner);
    if (out_effects) *out_effects |= effects;
    expr_info_list_record(&ctx->expr_info, expr, type, effects);
    return type;
}

static NovaTypeId analyze_literal(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    (void)scope;
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId type = ctx->type_unknown;
    switch (expr->as.literal.kind) {
    case NOVA_LITERAL_NUMBER:
        type = ctx->type_number;
        break;
    case NOVA_LITERAL_STRING:
        type = ctx->type_string;
        break;
    case NOVA_LITERAL_BOOL:
        type = ctx->type_bool;
        break;
    case NOVA_LITERAL_UNIT:
        type = ctx->type_unit;
        break;
    case NOVA_LITERAL_LIST: {
        NovaTypeId element = ctx->type_unknown;
        for (size_t i = 0; i < expr->as.literal.elements.count; ++i) {
            NovaEffectMask elem_effects = NOVA_EFFECT_NONE;
            NovaTypeId elem_type = analyze_expr(ctx, scope, expr->as.literal.elements.items[i], &elem_effects);
            element = unify_types(ctx, element, elem_type, expr->start_token);
            effects |= elem_effects;
        }
        type = type_list(ctx, element);
        break;
    }
    }
    if (out_effects) *out_effects |= effects;
    expr_info_list_record(&ctx->expr_info, expr, type, effects);
    return type;
}

static NovaTypeId analyze_identifier(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaScopeEntry *entry = scope_lookup(scope, &expr->as.identifier.name);
    if (!entry) {
        diagnostics_error(ctx, expr->as.identifier.name, "undefined identifier");
        expr_info_list_record(&ctx->expr_info, expr, ctx->type_unknown, NOVA_EFFECT_NONE);
        return ctx->type_unknown;
    }
    if (out_effects) *out_effects |= entry->effects;
    expr_info_list_record(&ctx->expr_info, expr, entry->type, entry->effects);
    return entry->type;
}

static NovaTypeId analyze_call(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    const NovaExpr *callee_expr = expr->as.call.callee;
    NovaEffectMask callee_effects = NOVA_EFFECT_NONE;
    NovaTypeId callee_type = analyze_expr(ctx, scope, callee_expr, &callee_effects);
    NovaEffectMask effects = callee_effects;
    NovaTypeInfo callee_info = ctx->types[callee_type];
    if (callee_info.kind != NOVA_TYPE_KIND_FUNCTION) {
        diagnostics_error(ctx, callee_expr->start_token, "attempted to call a non-function value");
        expr_info_list_record(&ctx->expr_info, expr, ctx->type_unknown, effects);
        if (out_effects) *out_effects |= effects;
        return ctx->type_unknown;
    }
    size_t arg_count = expr->as.call.args.count;
    if (callee_info.as.function.param_count != arg_count) {
        diagnostics_error(ctx, expr->start_token, "argument count mismatch");
    }
    for (size_t i = 0; i < arg_count; ++i) {
        NovaEffectMask arg_effects = NOVA_EFFECT_NONE;
        NovaTypeId arg_type = analyze_expr(ctx, scope, expr->as.call.args.items[i].value, &arg_effects);
        effects |= arg_effects;
        if (i < callee_info.as.function.param_count) {
            unify_types(ctx, callee_info.as.function.params[i], arg_type, expr->as.call.args.items[i].value->start_token);
        }
    }
    effects |= callee_info.as.function.effects;
    NovaTypeId result = callee_info.as.function.result;
    expr_info_list_record(&ctx->expr_info, expr, result, effects);
    if (out_effects) *out_effects |= effects;
    return result;
}

static NovaTypeId analyze_pipeline(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaEffectMask total_effects = NOVA_EFFECT_NONE;
    NovaTypeId current_type = analyze_expr(ctx, scope, expr->as.pipe.target, &total_effects);
    for (size_t i = 0; i < expr->as.pipe.stages.count; ++i) {
        const NovaExpr *stage = expr->as.pipe.stages.items[i];
        NovaEffectMask stage_effects = NOVA_EFFECT_NONE;
        const NovaExpr *callee = stage;
        NovaArgList args = { .items = NULL, .count = 0, .capacity = 0 };
        if (stage->kind == NOVA_EXPR_CALL) {
            callee = stage->as.call.callee;
            args = stage->as.call.args;
        }
        NovaTypeId callee_type = analyze_expr(ctx, scope, callee, &stage_effects);
        NovaTypeInfo callee_info = ctx->types[callee_type];
        if (callee_info.kind != NOVA_TYPE_KIND_FUNCTION || callee_info.as.function.param_count == 0) {
            diagnostics_error(ctx, stage->start_token, "pipeline stage is not callable");
            current_type = ctx->type_unknown;
            total_effects |= stage_effects;
            expr_info_list_record(&ctx->expr_info, stage, current_type, total_effects);
            continue;
        }
        size_t total_args = args.count + 1;
        if (callee_info.as.function.param_count < total_args) {
            diagnostics_error(ctx, stage->start_token, "pipeline stage expects fewer arguments");
        }
        unify_types(ctx, callee_info.as.function.params[0], current_type, stage->start_token);
        for (size_t j = 0; j < args.count; ++j) {
            NovaEffectMask arg_effects = NOVA_EFFECT_NONE;
            NovaTypeId arg_type = analyze_expr(ctx, scope, args.items[j].value, &arg_effects);
            stage_effects |= arg_effects;
            if (j + 1 < callee_info.as.function.param_count) {
                unify_types(ctx, callee_info.as.function.params[j + 1], arg_type, args.items[j].value->start_token);
            }
        }
        stage_effects |= callee_info.as.function.effects;
        current_type = callee_info.as.function.result;
        total_effects |= stage_effects;
        expr_info_list_record(&ctx->expr_info, stage, current_type, stage_effects);
    }
    expr_info_list_record(&ctx->expr_info, expr, current_type, total_effects);
    if (out_effects) *out_effects |= total_effects;
    return current_type;
}

static void check_match_exhaustiveness(NovaSemanticContext *ctx, const NovaExpr *expr, NovaTypeId scrutinee_type) {
    const NovaTypeInfo *info = &ctx->types[scrutinee_type];
    if (info->kind != NOVA_TYPE_KIND_CUSTOM || !info->as.custom.record) {
        return;
    }
    const NovaTypeRecord *record = info->as.custom.record;
    if (record->variant_count == 0) {
        return;
    }
    bool *seen = calloc(record->variant_count, sizeof(bool));
    size_t covered = 0;
    for (size_t i = 0; i < expr->as.match_expr.arms.count; ++i) {
        const NovaMatchArm *arm = &expr->as.match_expr.arms.items[i];
        for (size_t v = 0; v < record->variant_count; ++v) {
            if (token_equals(&record->variants[v].variant->name, &arm->name)) {
                if (!seen[v]) {
                    seen[v] = true;
                    covered++;
                }
            }
        }
    }
    if (covered < record->variant_count) {
        diagnostics_warning(ctx, expr->start_token, "match expression may be non-exhaustive");
    }
    free(seen);
}

static NovaTypeId analyze_match(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId scrutinee_type = analyze_expr(ctx, scope, expr->as.match_expr.scrutinee, &effects);
    NovaTypeId arm_type = ctx->type_unknown;
    for (size_t i = 0; i < expr->as.match_expr.arms.count; ++i) {
        const NovaMatchArm *arm = &expr->as.match_expr.arms.items[i];
        NovaScope *arm_scope = scope_push(scope);
        if (arm->bindings.count != 0) {
            const NovaTypeInfo *info = &ctx->types[scrutinee_type];
            if (info->kind == NOVA_TYPE_KIND_CUSTOM && info->as.custom.record) {
                const NovaTypeRecord *record = info->as.custom.record;
                const NovaVariantDecl *variant_decl = NULL;
                for (size_t v = 0; v < record->variant_count; ++v) {
                    if (token_equals(&record->variants[v].variant->name, &arm->name)) {
                        variant_decl = record->variants[v].variant;
                        break;
                    }
                }
                if (variant_decl && variant_decl->payload.count == arm->bindings.count) {
                    for (size_t p = 0; p < arm->bindings.count; ++p) {
                        NovaTypeId bind_type = ctx->type_unknown;
                        if (variant_decl->payload.items[p].has_type) {
                            bind_type = resolve_type_token(ctx, &variant_decl->payload.items[p].type_name);
                        }
                        scope_define(ctx, arm_scope, (NovaScopeEntry){
                            .name = arm->bindings.items[p].name,
                            .type = bind_type,
                            .effects = NOVA_EFFECT_NONE,
                        });
                    }
                }
            }
        }
        NovaEffectMask body_effects = NOVA_EFFECT_NONE;
        NovaTypeId body_type = analyze_expr(ctx, arm_scope, arm->body, &body_effects);
        scope_free(arm_scope);
        effects |= body_effects;
        arm_type = unify_types(ctx, arm_type, body_type, arm->body->start_token);
    }
    check_match_exhaustiveness(ctx, expr, scrutinee_type);
    expr_info_list_record(&ctx->expr_info, expr, arm_type, effects);
    if (out_effects) *out_effects |= effects;
    return arm_type;
}

static NovaTypeId analyze_if(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId cond_type = analyze_expr(ctx, scope, expr->as.if_expr.condition, &effects);
    if (cond_type != ctx->type_bool && cond_type != ctx->type_unknown) {
        diagnostics_error(ctx, expr->as.if_expr.condition->start_token, "if condition must be Bool");
    }
    NovaEffectMask then_effects = NOVA_EFFECT_NONE;
    NovaTypeId then_type = analyze_expr(ctx, scope, expr->as.if_expr.then_branch, &then_effects);
    effects |= then_effects;
    NovaTypeId else_type = ctx->type_unit;
    if (expr->as.if_expr.else_branch) {
        NovaEffectMask else_effects = NOVA_EFFECT_NONE;
        else_type = analyze_expr(ctx, scope, expr->as.if_expr.else_branch, &else_effects);
        effects |= else_effects;
    }
    NovaTypeId result = unify_types(ctx, then_type, else_type, expr->start_token);
    expr_info_list_record(&ctx->expr_info, expr, result, effects);
    if (out_effects) *out_effects |= effects;
    return result;
}

static NovaTypeId analyze_while(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId condition_type = analyze_expr(ctx, scope, expr->as.while_expr.condition, &effects);
    unify_types(ctx, ctx->type_bool, condition_type, expr->start_token);
    NovaEffectMask body_effects = NOVA_EFFECT_NONE;
    analyze_expr(ctx, scope, expr->as.while_expr.body, &body_effects);
    effects |= body_effects;
    expr_info_list_record(&ctx->expr_info, expr, ctx->type_unit, effects);
    if (out_effects) *out_effects |= effects;
    return ctx->type_unit;
}

static NovaTypeId analyze_lambda(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    NovaScope *lambda_scope = scope_push(scope);
    NovaTypeId *param_types = NULL;
    if (expr->as.lambda.params.count > 0) {
        param_types = malloc(expr->as.lambda.params.count * sizeof(NovaTypeId));
    }
    for (size_t i = 0; i < expr->as.lambda.params.count; ++i) {
        NovaTypeId param_type = ctx->type_unknown;
        if (expr->as.lambda.params.items[i].has_type) {
            param_type = resolve_type_token(ctx, &expr->as.lambda.params.items[i].type_name);
        }
        if (param_types) param_types[i] = param_type;
        scope_define(ctx, lambda_scope, (NovaScopeEntry){
            .name = expr->as.lambda.params.items[i].name,
            .type = param_type,
            .effects = NOVA_EFFECT_NONE,
        });
    }
    NovaEffectMask body_effects = NOVA_EFFECT_NONE;
    NovaTypeId body_type = analyze_expr(ctx, lambda_scope, expr->as.lambda.body, &body_effects);
    scope_free(lambda_scope);
    NovaTypeId fn_type = type_function(ctx, param_types, expr->as.lambda.params.count, body_type, body_effects);
    free(param_types);
    expr_info_list_record(&ctx->expr_info, expr, fn_type, NOVA_EFFECT_NONE);
    if (out_effects) *out_effects |= NOVA_EFFECT_NONE;
    return fn_type;
}

static NovaTypeId analyze_expr(NovaSemanticContext *ctx, NovaScope *scope, const NovaExpr *expr, NovaEffectMask *out_effects) {
    if (!expr) {
        if (out_effects) *out_effects |= NOVA_EFFECT_NONE;
        return ctx->type_unknown;
    }
    switch (expr->kind) {
    case NOVA_EXPR_LITERAL:
    case NOVA_EXPR_LIST_LITERAL:
        return analyze_literal(ctx, scope, expr, out_effects);
    case NOVA_EXPR_IDENTIFIER:
        return analyze_identifier(ctx, scope, expr, out_effects);
    case NOVA_EXPR_BLOCK:
        return analyze_block(ctx, scope, expr, out_effects);
    case NOVA_EXPR_LAMBDA:
        return analyze_lambda(ctx, scope, expr, out_effects);
    case NOVA_EXPR_CALL:
        return analyze_call(ctx, scope, expr, out_effects);
    case NOVA_EXPR_PIPE:
        return analyze_pipeline(ctx, scope, expr, out_effects);
    case NOVA_EXPR_IF:
        return analyze_if(ctx, scope, expr, out_effects);
    case NOVA_EXPR_WHILE:
        return analyze_while(ctx, scope, expr, out_effects);
    case NOVA_EXPR_MATCH:
        return analyze_match(ctx, scope, expr, out_effects);
    case NOVA_EXPR_ASYNC: {
        NovaEffectMask effects = NOVA_EFFECT_ASYNC;
        NovaTypeId inner = analyze_expr(ctx, scope, expr->as.unary.value, &effects);
        if (out_effects) *out_effects |= effects;
        expr_info_list_record(&ctx->expr_info, expr, inner, effects);
        return inner;
    }
    case NOVA_EXPR_AWAIT: {
        NovaEffectMask effects = NOVA_EFFECT_NONE;
        NovaTypeId inner = analyze_expr(ctx, scope, expr->as.unary.value, &effects);
        if (out_effects) *out_effects |= effects;
        expr_info_list_record(&ctx->expr_info, expr, inner, effects);
        return inner;
    }
    case NOVA_EXPR_EFFECT: {
        NovaEffectMask effects = NOVA_EFFECT_IMPURE;
        NovaTypeId inner = analyze_expr(ctx, scope, expr->as.unary.value, &effects);
        effects |= NOVA_EFFECT_IMPURE;
        if (out_effects) *out_effects |= effects;
        expr_info_list_record(&ctx->expr_info, expr, inner, effects);
        return inner;
    }
    case NOVA_EXPR_PAREN:
        return analyze_expr(ctx, scope, expr->as.inner, out_effects);
    }
    return ctx->type_unknown;
}

static void analyze_let(NovaSemanticContext *ctx, NovaScope *scope, const NovaLetDecl *decl) {
    NovaEffectMask effects = NOVA_EFFECT_NONE;
    NovaTypeId value_type = analyze_expr(ctx, scope, decl->value, &effects);
    if (decl->has_type) {
        NovaTypeId annotation = resolve_type_token(ctx, &decl->type_name);
        value_type = unify_types(ctx, annotation, value_type, decl->type_name);
    }
    scope_define(ctx, scope, (NovaScopeEntry){
        .name = decl->name,
        .type = value_type,
        .effects = effects,
    });
}

static void analyze_fun(NovaSemanticContext *ctx, NovaScope *scope, const NovaFunDecl *decl) {
    NovaTypeId *param_types = NULL;
    if (decl->params.count > 0) {
        param_types = malloc(decl->params.count * sizeof(NovaTypeId));
    }
    for (size_t i = 0; i < decl->params.count; ++i) {
        if (decl->params.items[i].has_type) {
            param_types[i] = resolve_type_token(ctx, &decl->params.items[i].type_name);
        } else {
            param_types[i] = ctx->type_unknown;
        }
    }
    NovaTypeId return_type = ctx->type_unknown;
    if (decl->has_return_type) {
        return_type = resolve_type_token(ctx, &decl->return_type);
    }
    NovaTypeId function_type = type_function(ctx, param_types, decl->params.count, return_type, NOVA_EFFECT_NONE);
    scope_define(ctx, scope, (NovaScopeEntry){
        .name = decl->name,
        .type = function_type,
        .effects = NOVA_EFFECT_NONE,
    });
    NovaScope *fn_scope = scope_push(scope);
    for (size_t i = 0; i < decl->params.count; ++i) {
        scope_define(ctx, fn_scope, (NovaScopeEntry){
            .name = decl->params.items[i].name,
            .type = param_types[i],
            .effects = NOVA_EFFECT_NONE,
        });
    }
    NovaEffectMask body_effects = NOVA_EFFECT_NONE;
    NovaTypeId body_type = analyze_expr(ctx, fn_scope, decl->body, &body_effects);
    scope_free(fn_scope);
    ctx->types[function_type].as.function.result = decl->has_return_type ? return_type : body_type;
    ctx->types[function_type].as.function.effects = body_effects;
    free(param_types);
}

void nova_semantic_context_init(NovaSemanticContext *ctx) {
    ctx->scope = scope_push(NULL);
    nova_diagnostic_list_init(&ctx->diagnostics);
    ctx->types = NULL;
    ctx->type_count = 0;
    ctx->type_capacity = 0;
    type_record_list_init(&ctx->type_records);
    expr_info_list_init(&ctx->expr_info);
    ctx->type_unknown = type_pool_add(ctx, (NovaTypeInfo){ .kind = NOVA_TYPE_KIND_UNKNOWN });
    ctx->type_unit = type_pool_add(ctx, (NovaTypeInfo){ .kind = NOVA_TYPE_KIND_UNIT });
    ctx->type_number = type_pool_add(ctx, (NovaTypeInfo){ .kind = NOVA_TYPE_KIND_NUMBER });
    ctx->type_string = type_pool_add(ctx, (NovaTypeInfo){ .kind = NOVA_TYPE_KIND_STRING });
    ctx->type_bool = type_pool_add(ctx, (NovaTypeInfo){ .kind = NOVA_TYPE_KIND_BOOL });
}

void nova_semantic_context_free(NovaSemanticContext *ctx) {
    if (ctx->scope) {
        scope_free(ctx->scope);
    }
    for (size_t i = 0; i < ctx->type_count; ++i) {
        if (ctx->types[i].kind == NOVA_TYPE_KIND_FUNCTION) {
            free(ctx->types[i].as.function.params);
        }
    }
    free(ctx->types);
    type_record_list_free(&ctx->type_records);
    expr_info_list_free(&ctx->expr_info);
    nova_diagnostic_list_free(&ctx->diagnostics);
}

void nova_semantic_analyze_program(NovaSemanticContext *ctx, const NovaProgram *program) {
    for (size_t i = 0; i < program->decl_count; ++i) {
        if (program->decls[i].kind == NOVA_DECL_TYPE) {
            register_type_decl(ctx, &program->decls[i].as.type_decl);
        }
    }
    for (size_t i = 0; i < program->decl_count; ++i) {
        const NovaDecl *decl = &program->decls[i];
        switch (decl->kind) {
        case NOVA_DECL_LET:
            analyze_let(ctx, ctx->scope, &decl->as.let_decl);
            break;
        case NOVA_DECL_FUN:
            analyze_fun(ctx, ctx->scope, &decl->as.fun_decl);
            break;
        case NOVA_DECL_TYPE:
            break;
        }
    }
}

const NovaExprInfo *nova_semantic_lookup_expr(const NovaSemanticContext *ctx, const NovaExpr *expr) {
    for (size_t i = 0; i < ctx->expr_info.count; ++i) {
        if (ctx->expr_info.items[i].expr == expr) {
            return &ctx->expr_info.items[i];
        }
    }
    return NULL;
}

const NovaTypeInfo *nova_semantic_type_info(const NovaSemanticContext *ctx, NovaTypeId type_id) {
    if (type_id >= ctx->type_count) {
        return NULL;
    }
    return &ctx->types[type_id];
}

const NovaTypeRecord *nova_semantic_find_type(const NovaSemanticContext *ctx, const NovaToken *name) {
    return type_record_find(ctx, name);
}
