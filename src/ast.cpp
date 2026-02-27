#include "nova/ast.h"

#include <stdlib.h>

static void *nova_grow(void *ptr, size_t *capacity, size_t element_size) {
    size_t new_capacity = *capacity == 0 ? 4 : (*capacity * 2);
    void *new_ptr = realloc(ptr, new_capacity * element_size);
    if (!new_ptr) {
        return NULL;
    }
    *capacity = new_capacity;
    return new_ptr;
}

void nova_param_list_init(NovaParamList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_param_list_push(NovaParamList *list, NovaParam param) {
    if (list->count == list->capacity) {
        NovaParam *items = nova_grow(list->items, &list->capacity, sizeof(NovaParam));
        if (!items) {
            return;
        }
        list->items = items;
    }
    list->items[list->count++] = param;
}

void nova_param_list_free(NovaParamList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_arg_list_init(NovaArgList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_arg_list_push(NovaArgList *list, NovaArg arg) {
    if (list->count == list->capacity) {
        NovaArg *items = nova_grow(list->items, &list->capacity, sizeof(NovaArg));
        if (!items) {
            return;
        }
        list->items = items;
    }
    list->items[list->count++] = arg;
}

void nova_arg_list_free(NovaArgList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_expr_list_init(NovaExprList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_expr_list_push(NovaExprList *list, NovaExpr *expr) {
    if (list->count == list->capacity) {
        NovaExpr **items = nova_grow(list->items, &list->capacity, sizeof(NovaExpr *));
        if (!items) {
            return;
        }
        list->items = items;
    }
    list->items[list->count++] = expr;
}

void nova_expr_list_free(NovaExprList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_match_arm_list_init(NovaMatchArmList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_match_arm_list_push(NovaMatchArmList *list, NovaMatchArm arm) {
    if (list->count == list->capacity) {
        NovaMatchArm *items = nova_grow(list->items, &list->capacity, sizeof(NovaMatchArm));
        if (!items) {
            return;
        }
        list->items = items;
    }
    list->items[list->count++] = arm;
}

void nova_match_arm_list_free(NovaMatchArmList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        nova_param_list_free(&list->items[i].bindings);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_variant_list_init(NovaVariantList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_variant_list_push(NovaVariantList *list, NovaVariantDecl variant) {
    if (list->count == list->capacity) {
        NovaVariantDecl *items = nova_grow(list->items, &list->capacity, sizeof(NovaVariantDecl));
        if (!items) {
            return;
        }
        list->items = items;
    }
    list->items[list->count++] = variant;
}

void nova_variant_list_free(NovaVariantList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        nova_param_list_free(&list->items[i].payload);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void nova_module_path_init(NovaModulePath *path) {
    path->segments = NULL;
    path->count = 0;
    path->capacity = 0;
}

void nova_module_path_push(NovaModulePath *path, NovaToken segment) {
    if (path->count == path->capacity) {
        NovaToken *segments = nova_grow(path->segments, &path->capacity, sizeof(NovaToken));
        if (!segments) {
            return;
        }
        path->segments = segments;
    }
    path->segments[path->count++] = segment;
}

void nova_module_path_free(NovaModulePath *path) {
    free(path->segments);
    path->segments = NULL;
    path->count = 0;
    path->capacity = 0;
}

static void nova_expr_free(NovaExpr *expr);

static void nova_literal_free(NovaLiteral *literal) {
    if (literal->kind == NOVA_LITERAL_LIST) {
        for (size_t i = 0; i < literal->elements.count; ++i) {
            nova_expr_free(literal->elements.items[i]);
        }
        nova_expr_list_free(&literal->elements);
    }
}

static void nova_expr_free(NovaExpr *expr) {
    if (!expr) {
        return;
    }
    switch (expr->kind) {
    case NOVA_EXPR_IF:
        nova_expr_free(expr->as.if_expr.condition);
        nova_expr_free(expr->as.if_expr.then_branch);
        nova_expr_free(expr->as.if_expr.else_branch);
        break;
    case NOVA_EXPR_WHILE:
        nova_expr_free(expr->as.while_expr.condition);
        nova_expr_free(expr->as.while_expr.body);
        break;
    case NOVA_EXPR_MATCH:
        nova_expr_free(expr->as.match_expr.scrutinee);
        for (size_t i = 0; i < expr->as.match_expr.arms.count; ++i) {
            NovaMatchArm *arm = &expr->as.match_expr.arms.items[i];
            nova_expr_free(arm->body);
        }
        nova_match_arm_list_free(&expr->as.match_expr.arms);
        break;
    case NOVA_EXPR_ASYNC:
    case NOVA_EXPR_EFFECT:
    case NOVA_EXPR_AWAIT:
        nova_expr_free(expr->as.unary.value);
        break;
    case NOVA_EXPR_PIPE:
        nova_expr_free(expr->as.pipe.target);
        for (size_t i = 0; i < expr->as.pipe.stages.count; ++i) {
            nova_expr_free(expr->as.pipe.stages.items[i]);
        }
        nova_expr_list_free(&expr->as.pipe.stages);
        break;
    case NOVA_EXPR_CALL:
        nova_expr_free(expr->as.call.callee);
        for (size_t i = 0; i < expr->as.call.args.count; ++i) {
            nova_expr_free(expr->as.call.args.items[i].value);
        }
        nova_arg_list_free(&expr->as.call.args);
        break;
    case NOVA_EXPR_LITERAL:
    case NOVA_EXPR_LIST_LITERAL:
        nova_literal_free(&expr->as.literal);
        break;
    case NOVA_EXPR_LAMBDA:
        nova_param_list_free(&expr->as.lambda.params);
        nova_expr_free(expr->as.lambda.body);
        break;
    case NOVA_EXPR_BLOCK:
        for (size_t i = 0; i < expr->as.block.expressions.count; ++i) {
            nova_expr_free(expr->as.block.expressions.items[i]);
        }
        nova_expr_list_free(&expr->as.block.expressions);
        break;
    case NOVA_EXPR_PAREN:
        nova_expr_free(expr->as.inner);
        break;
    case NOVA_EXPR_IDENTIFIER:
        break;
    }
    free(expr);
}

void nova_program_init(NovaProgram *program) {
    nova_module_path_init(&program->module_decl.path);
    program->imports = NULL;
    program->import_count = 0;
    program->import_capacity = 0;
    program->decls = NULL;
    program->decl_count = 0;
    program->decl_capacity = 0;
}

void nova_program_add_import(NovaProgram *program, NovaImportDecl import) {
    if (program->import_count == program->import_capacity) {
        NovaImportDecl *imports = nova_grow(program->imports, &program->import_capacity, sizeof(NovaImportDecl));
        if (!imports) {
            return;
        }
        program->imports = imports;
    }
    program->imports[program->import_count++] = import;
}

void nova_program_add_decl(NovaProgram *program, NovaDecl decl) {
    if (program->decl_count == program->decl_capacity) {
        NovaDecl *decls = nova_grow(program->decls, &program->decl_capacity, sizeof(NovaDecl));
        if (!decls) {
            return;
        }
        program->decls = decls;
    }
    program->decls[program->decl_count++] = decl;
}

static void nova_import_free(NovaImportDecl *import) {
    nova_module_path_free(&import->path);
    free(import->symbols);
    import->symbols = NULL;
    import->symbol_count = 0;
    import->symbol_capacity = 0;
}

static void nova_decl_free(NovaDecl *decl) {
    switch (decl->kind) {
    case NOVA_DECL_LET:
        nova_expr_free(decl->as.let_decl.value);
        break;
    case NOVA_DECL_FUN:
        nova_param_list_free(&decl->as.fun_decl.params);
        nova_expr_free(decl->as.fun_decl.body);
        break;
    case NOVA_DECL_TYPE:
        if (decl->as.type_decl.kind == NOVA_TYPE_DECL_SUM) {
            nova_variant_list_free(&decl->as.type_decl.variants);
        } else {
            nova_param_list_free(&decl->as.type_decl.tuple_fields);
        }
        break;
    }
}

void nova_program_free(NovaProgram *program) {
    for (size_t i = 0; i < program->import_count; ++i) {
        nova_import_free(&program->imports[i]);
    }
    free(program->imports);
    for (size_t i = 0; i < program->decl_count; ++i) {
        nova_decl_free(&program->decls[i]);
    }
    free(program->decls);
    nova_module_path_free(&program->module_decl.path);
    program->imports = NULL;
    program->decls = NULL;
    program->import_count = 0;
    program->import_capacity = 0;
    program->decl_count = 0;
    program->decl_capacity = 0;
}
