#include "nova/ir.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static char *copy_token_text(const NovaToken *token) {
    char *text = malloc(token->length + 1);
    if (!text) return NULL;
    memcpy(text, token->lexeme, token->length);
    text[token->length] = '\0';
    return text;
}

static bool token_equals_cstr(const NovaToken *token, const char *text) {
    size_t len = strlen(text);
    return token && token->length == len && strncmp(token->lexeme, text, len) == 0;
}

static NovaIRExpr *nova_ir_expr_new(NovaIRExprKind kind, NovaTypeId type) {
    NovaIRExpr *expr = calloc(1, sizeof(NovaIRExpr));
    if (!expr) return NULL;
    expr->kind = kind;
    expr->type = type;
    return expr;
}

static NovaIRExpr *lower_expr(const NovaExpr *expr, const NovaSemanticContext *semantics);
static void nova_ir_expr_free(NovaIRExpr *expr);
static void optimize_ir_expr(NovaIRExpr **expr_ptr);

static NovaTypeId infer_type_from_token(const NovaSemanticContext *semantics, const NovaToken *token) {
    if (!token) return semantics->type_unknown;
    if (token_equals_cstr(token, "Number")) return semantics->type_number;
    if (token_equals_cstr(token, "String")) return semantics->type_string;
    if (token_equals_cstr(token, "Bool")) return semantics->type_bool;
    if (token_equals_cstr(token, "Unit")) return semantics->type_unit;
    const NovaTypeRecord *record = nova_semantic_find_type(semantics, token);
    if (record) return record->type_id;
    return semantics->type_unknown;
}

static NovaIRExpr *lower_literal(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
    NovaTypeId type = info ? info->type : 0;
    NovaIRExpr *ir = NULL;
    switch (expr->as.literal.kind) {
    case NOVA_LITERAL_NUMBER: {
        char buffer[64];
        size_t len = expr->as.literal.token.length;
        len = len < sizeof(buffer) - 1 ? len : sizeof(buffer) - 1;
        memcpy(buffer, expr->as.literal.token.lexeme, len);
        buffer[len] = '\0';
        ir = nova_ir_expr_new(NOVA_IR_EXPR_NUMBER, type);
        if (ir) {
            ir->as.number_value = strtod(buffer, NULL);
        }
        break;
    }
    case NOVA_LITERAL_STRING: {
        ir = nova_ir_expr_new(NOVA_IR_EXPR_STRING, type);
        if (ir) {
            ir->as.string_value.text = copy_token_text(&expr->as.literal.token);
        }
        break;
    }
    case NOVA_LITERAL_BOOL: {
        ir = nova_ir_expr_new(NOVA_IR_EXPR_BOOL, type);
        if (ir) {
            ir->as.bool_value = token_equals_cstr(&expr->as.literal.token, "true");
        }
        break;
    }
    case NOVA_LITERAL_UNIT:
        ir = nova_ir_expr_new(NOVA_IR_EXPR_UNIT, type);
        break;
    case NOVA_LITERAL_LIST: {
        ir = nova_ir_expr_new(NOVA_IR_EXPR_LIST, type);
        if (ir) {
            size_t count = expr->as.literal.elements.count;
            if (count > 0) {
                ir->as.list.elements = calloc(count, sizeof(NovaIRExpr *));
                if (!ir->as.list.elements) {
                    nova_ir_expr_free(ir);
                    return NULL;
                }
                ir->as.list.count = count;
                for (size_t i = 0; i < count; ++i) {
                    ir->as.list.elements[i] = lower_expr(expr->as.literal.elements.items[i], semantics);
                    if (!ir->as.list.elements[i]) {
                        nova_ir_expr_free(ir);
                        return NULL;
                    }
                }
            }
        }
        break;
    }
    }
    return ir;
}

static NovaIRExpr *lower_call(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
    NovaIRExpr *ir = nova_ir_expr_new(NOVA_IR_EXPR_CALL, info ? info->type : 0);
    if (!ir) return NULL;
    NovaExpr *callee_expr = expr->as.call.callee;
    if (callee_expr->kind != NOVA_EXPR_IDENTIFIER) {
        free(ir);
        return NULL;
    }
    ir->as.call.callee = callee_expr->as.identifier.name;
    ir->as.call.arg_count = expr->as.call.args.count;
    if (ir->as.call.arg_count > 0) {
        ir->as.call.args = calloc(ir->as.call.arg_count, sizeof(NovaIRExpr *));
        if (!ir->as.call.args) {
            free(ir);
            return NULL;
        }
        for (size_t i = 0; i < expr->as.call.args.count; ++i) {
            ir->as.call.args[i] = lower_expr(expr->as.call.args.items[i].value, semantics);
        }
    }
    return ir;
}

static NovaIRExpr *lower_if(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
    NovaIRExpr *ir = nova_ir_expr_new(NOVA_IR_EXPR_IF, info ? info->type : 0);
    if (!ir) return NULL;
    ir->as.if_expr.condition = lower_expr(expr->as.if_expr.condition, semantics);
    if (!ir->as.if_expr.condition) {
        nova_ir_expr_free(ir);
        return NULL;
    }
    ir->as.if_expr.then_branch = lower_expr(expr->as.if_expr.then_branch, semantics);
    if (!ir->as.if_expr.then_branch) {
        nova_ir_expr_free(ir);
        return NULL;
    }
    if (expr->as.if_expr.else_branch) {
        ir->as.if_expr.else_branch = lower_expr(expr->as.if_expr.else_branch, semantics);
        if (!ir->as.if_expr.else_branch) {
            nova_ir_expr_free(ir);
            return NULL;
        }
    } else {
        ir->as.if_expr.else_branch = nova_ir_expr_new(NOVA_IR_EXPR_UNIT, semantics->type_unit);
        if (!ir->as.if_expr.else_branch) {
            nova_ir_expr_free(ir);
            return NULL;
        }
    }
    return ir;
}

static NovaIRExpr *lower_while(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
    NovaIRExpr *ir = nova_ir_expr_new(NOVA_IR_EXPR_WHILE, info ? info->type : 0);
    if (!ir) return NULL;
    ir->as.while_expr.condition = lower_expr(expr->as.while_expr.condition, semantics);
    if (!ir->as.while_expr.condition) {
        nova_ir_expr_free(ir);
        return NULL;
    }
    ir->as.while_expr.body = lower_expr(expr->as.while_expr.body, semantics);
    if (!ir->as.while_expr.body) {
        nova_ir_expr_free(ir);
        return NULL;
    }
    return ir;
}

static NovaIRExpr *lower_pipeline(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    NovaIRExpr *current = lower_expr(expr->as.pipe.target, semantics);
    if (!current) {
        return NULL;
    }
    for (size_t i = 0; i < expr->as.pipe.stages.count; ++i) {
        const NovaExpr *stage = expr->as.pipe.stages.items[i];
        const NovaExpr *callee = stage;
        NovaArgList args = { .items = NULL, .count = 0, .capacity = 0 };
        if (stage->kind == NOVA_EXPR_CALL) {
            callee = stage->as.call.callee;
            args = stage->as.call.args;
        }
        if (!callee || callee->kind != NOVA_EXPR_IDENTIFIER) {
            nova_ir_expr_free(current);
            return NULL;
        }
        const NovaExprInfo *stage_info = nova_semantic_lookup_expr(semantics, stage);
        NovaIRExpr *call = nova_ir_expr_new(NOVA_IR_EXPR_CALL, stage_info ? stage_info->type : 0);
        if (!call) {
            nova_ir_expr_free(current);
            return NULL;
        }
        call->as.call.callee = callee->as.identifier.name;
        size_t arg_count = 1 + args.count;
        call->as.call.arg_count = arg_count;
        call->as.call.args = calloc(arg_count, sizeof(NovaIRExpr *));
        if (!call->as.call.args) {
            nova_ir_expr_free(call);
            return NULL;
        }
        call->as.call.args[0] = current;
        for (size_t a = 0; a < args.count; ++a) {
            call->as.call.args[a + 1] = lower_expr(args.items[a].value, semantics);
            if (!call->as.call.args[a + 1]) {
                nova_ir_expr_free(call);
                return NULL;
            }
        }
        current = call;
    }
    return current;
}

static NovaIRExpr *lower_match(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
    NovaIRExpr *ir = nova_ir_expr_new(NOVA_IR_EXPR_MATCH, info ? info->type : 0);
    if (!ir) return NULL;
    ir->as.match_expr.scrutinee = lower_expr(expr->as.match_expr.scrutinee, semantics);
    if (!ir->as.match_expr.scrutinee) {
        nova_ir_expr_free(ir);
        return NULL;
    }
    size_t arm_count = expr->as.match_expr.arms.count;
    ir->as.match_expr.arm_count = arm_count;
    if (arm_count > 0) {
        ir->as.match_expr.arms = calloc(arm_count, sizeof(NovaIRMatchArm));
        if (!ir->as.match_expr.arms) {
            nova_ir_expr_free(ir);
            return NULL;
        }
        for (size_t i = 0; i < arm_count; ++i) {
            const NovaMatchArm *arm = &expr->as.match_expr.arms.items[i];
            NovaIRMatchArm *ir_arm = &ir->as.match_expr.arms[i];
            ir_arm->constructor = arm->name;
            ir_arm->binding_count = arm->bindings.count;
            if (ir_arm->binding_count > 0) {
                ir_arm->bindings = calloc(ir_arm->binding_count, sizeof(NovaToken));
                if (!ir_arm->bindings) {
                    nova_ir_expr_free(ir);
                    return NULL;
                }
                for (size_t b = 0; b < ir_arm->binding_count; ++b) {
                    ir_arm->bindings[b] = arm->bindings.items[b].name;
                }
            }
            ir_arm->body = lower_expr(arm->body, semantics);
            if (!ir_arm->body) {
                nova_ir_expr_free(ir);
                return NULL;
            }
        }
    }
    return ir;
}

static NovaIRExpr *lower_expr(const NovaExpr *expr, const NovaSemanticContext *semantics) {
    if (!expr) return NULL;
    switch (expr->kind) {
    case NOVA_EXPR_LITERAL:
    case NOVA_EXPR_LIST_LITERAL:
        return lower_literal(expr, semantics);
    case NOVA_EXPR_IDENTIFIER: {
        const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
        NovaIRExpr *ir = nova_ir_expr_new(NOVA_IR_EXPR_IDENTIFIER, info ? info->type : 0);
        if (ir) {
            ir->as.identifier = expr->as.identifier.name;
        }
        return ir;
    }
    case NOVA_EXPR_CALL:
        return lower_call(expr, semantics);
    case NOVA_EXPR_PIPE:
        return lower_pipeline(expr, semantics);
    case NOVA_EXPR_IF:
        return lower_if(expr, semantics);
    case NOVA_EXPR_WHILE:
        return lower_while(expr, semantics);
    case NOVA_EXPR_BLOCK: {
        if (expr->as.block.expressions.count == 0) {
            return nova_ir_expr_new(NOVA_IR_EXPR_UNIT, semantics->type_unit);
        }
        if (expr->as.block.expressions.count == 1) {
            return lower_expr(expr->as.block.expressions.items[0], semantics);
        }
        const NovaExprInfo *info = nova_semantic_lookup_expr(semantics, expr);
        NovaIRExpr *sequence = nova_ir_expr_new(NOVA_IR_EXPR_SEQUENCE, info ? info->type : 0);
        if (!sequence) return NULL;
        size_t count = expr->as.block.expressions.count;
        sequence->as.sequence.count = count;
        sequence->as.sequence.items = calloc(count, sizeof(NovaIRExpr *));
        if (!sequence->as.sequence.items) {
            nova_ir_expr_free(sequence);
            return NULL;
        }
        for (size_t i = 0; i < count; ++i) {
            sequence->as.sequence.items[i] = lower_expr(expr->as.block.expressions.items[i], semantics);
            if (!sequence->as.sequence.items[i]) {
                nova_ir_expr_free(sequence);
                return NULL;
            }
        }
        return sequence;
    }
    case NOVA_EXPR_PAREN:
        return lower_expr(expr->as.inner, semantics);
    case NOVA_EXPR_MATCH:
        return lower_match(expr, semantics);
    case NOVA_EXPR_ASYNC:
    case NOVA_EXPR_AWAIT:
    case NOVA_EXPR_EFFECT:
        return lower_expr(expr->as.unary.value, semantics);
    default:
        return NULL;
    }
}

static bool ir_expr_is_bool_constant(const NovaIRExpr *expr, bool *value) {
    if (!expr || expr->kind != NOVA_IR_EXPR_BOOL) {
        return false;
    }
    if (value) {
        *value = expr->as.bool_value;
    }
    return true;
}

static void optimize_ir_expr(NovaIRExpr **expr_ptr) {
    if (!expr_ptr || !*expr_ptr) {
        return;
    }
    NovaIRExpr *expr = *expr_ptr;
    switch (expr->kind) {
    case NOVA_IR_EXPR_CALL:
        for (size_t i = 0; i < expr->as.call.arg_count; ++i) {
            optimize_ir_expr(&expr->as.call.args[i]);
        }
        break;
    case NOVA_IR_EXPR_SEQUENCE:
        for (size_t i = 0; i < expr->as.sequence.count; ++i) {
            optimize_ir_expr(&expr->as.sequence.items[i]);
        }
        if (expr->as.sequence.count == 0) {
            free(expr->as.sequence.items);
            NovaIRExpr temp = { .kind = NOVA_IR_EXPR_UNIT, .type = expr->type };
            *expr = temp;
        } else if (expr->as.sequence.count == 1) {
            NovaIRExpr *only = expr->as.sequence.items[0];
            free(expr->as.sequence.items);
            NovaIRExpr temp = *only;
            free(only);
            *expr = temp;
            optimize_ir_expr(expr_ptr);
            return;
        }
        break;
    case NOVA_IR_EXPR_LIST:
        for (size_t i = 0; i < expr->as.list.count; ++i) {
            optimize_ir_expr(&expr->as.list.elements[i]);
        }
        break;
    case NOVA_IR_EXPR_IF: {
        optimize_ir_expr(&expr->as.if_expr.condition);
        optimize_ir_expr(&expr->as.if_expr.then_branch);
        optimize_ir_expr(&expr->as.if_expr.else_branch);
        bool condition_value = false;
        if (ir_expr_is_bool_constant(expr->as.if_expr.condition, &condition_value)) {
            NovaIRExpr *selected = condition_value ? expr->as.if_expr.then_branch : expr->as.if_expr.else_branch;
            NovaIRExpr *discard = condition_value ? expr->as.if_expr.else_branch : expr->as.if_expr.then_branch;
            NovaIRExpr *condition = expr->as.if_expr.condition;
            expr->as.if_expr.condition = NULL;
            expr->as.if_expr.then_branch = NULL;
            expr->as.if_expr.else_branch = NULL;
            nova_ir_expr_free(condition);
            nova_ir_expr_free(discard);
            NovaIRExpr temp = *selected;
            free(selected);
            *expr = temp;
            optimize_ir_expr(expr_ptr);
            return;
        }
        break;
    }
    case NOVA_IR_EXPR_WHILE: {
        optimize_ir_expr(&expr->as.while_expr.condition);
        optimize_ir_expr(&expr->as.while_expr.body);
        bool condition_value = false;
        if (ir_expr_is_bool_constant(expr->as.while_expr.condition, &condition_value) && !condition_value) {
            nova_ir_expr_free(expr->as.while_expr.condition);
            nova_ir_expr_free(expr->as.while_expr.body);
            expr->as.while_expr.condition = NULL;
            expr->as.while_expr.body = NULL;
            expr->kind = NOVA_IR_EXPR_UNIT;
        }
        break;
    }
    case NOVA_IR_EXPR_MATCH:
        optimize_ir_expr(&expr->as.match_expr.scrutinee);
        for (size_t i = 0; i < expr->as.match_expr.arm_count; ++i) {
            optimize_ir_expr(&expr->as.match_expr.arms[i].body);
        }
        if (expr->as.match_expr.arm_count == 1 && expr->as.match_expr.arms) {
            NovaIRMatchArm *arm = &expr->as.match_expr.arms[0];
            if (arm->binding_count == 0) {
                NovaIRExpr *scrutinee = expr->as.match_expr.scrutinee;
                NovaIRExpr *body = arm->body;
                free(arm->bindings);
                free(expr->as.match_expr.arms);
                expr->as.match_expr.arms = NULL;
                expr->as.match_expr.arm_count = 0;
                expr->as.match_expr.scrutinee = NULL;
                nova_ir_expr_free(scrutinee);
                NovaIRExpr temp = *body;
                free(body);
                *expr = temp;
                optimize_ir_expr(expr_ptr);
                return;
            }
        }
        break;
    case NOVA_IR_EXPR_NUMBER:
    case NOVA_IR_EXPR_STRING:
    case NOVA_IR_EXPR_BOOL:
    case NOVA_IR_EXPR_UNIT:
    case NOVA_IR_EXPR_IDENTIFIER:
        break;
    }
}

static void nova_ir_function_init(NovaIRFunction *fn) {
    fn->params = NULL;
    fn->param_count = 0;
    fn->body = NULL;
}

static void nova_ir_expr_free(NovaIRExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
    case NOVA_IR_EXPR_STRING:
        free(expr->as.string_value.text);
        break;
    case NOVA_IR_EXPR_LIST:
        if (expr->as.list.elements) {
            for (size_t i = 0; i < expr->as.list.count; ++i) {
                nova_ir_expr_free(expr->as.list.elements[i]);
            }
            free(expr->as.list.elements);
        }
        break;
    case NOVA_IR_EXPR_SEQUENCE:
        if (expr->as.sequence.items) {
            for (size_t i = 0; i < expr->as.sequence.count; ++i) {
                nova_ir_expr_free(expr->as.sequence.items[i]);
            }
            free(expr->as.sequence.items);
        }
        break;
    case NOVA_IR_EXPR_CALL:
        if (expr->as.call.args) {
            for (size_t i = 0; i < expr->as.call.arg_count; ++i) {
                nova_ir_expr_free(expr->as.call.args[i]);
            }
            free(expr->as.call.args);
        }
        break;
    case NOVA_IR_EXPR_IF:
        nova_ir_expr_free(expr->as.if_expr.condition);
        nova_ir_expr_free(expr->as.if_expr.then_branch);
        nova_ir_expr_free(expr->as.if_expr.else_branch);
        break;
    case NOVA_IR_EXPR_WHILE:
        nova_ir_expr_free(expr->as.while_expr.condition);
        nova_ir_expr_free(expr->as.while_expr.body);
        break;
    case NOVA_IR_EXPR_MATCH:
        nova_ir_expr_free(expr->as.match_expr.scrutinee);
        if (expr->as.match_expr.arms) {
            for (size_t i = 0; i < expr->as.match_expr.arm_count; ++i) {
                free(expr->as.match_expr.arms[i].bindings);
                nova_ir_expr_free(expr->as.match_expr.arms[i].body);
            }
            free(expr->as.match_expr.arms);
        }
        break;
    default:
        break;
    }
    free(expr);
}

static void nova_ir_function_free(NovaIRFunction *fn) {
    free(fn->params);
    nova_ir_expr_free(fn->body);
}

NovaIRProgram *nova_ir_lower(const NovaProgram *program, const NovaSemanticContext *semantics) {
    NovaIRProgram *ir = calloc(1, sizeof(NovaIRProgram));
    if (!ir) return NULL;
    for (size_t i = 0; i < program->decl_count; ++i) {
        const NovaDecl *decl = &program->decls[i];
        if (decl->kind != NOVA_DECL_FUN) continue;
        if (ir->function_count == ir->function_capacity) {
            size_t new_capacity = ir->function_capacity == 0 ? 4 : ir->function_capacity * 2;
            NovaIRFunction *functions = realloc(ir->functions, new_capacity * sizeof(NovaIRFunction));
            if (!functions) {
                continue;
            }
            ir->functions = functions;
            ir->function_capacity = new_capacity;
        }
        NovaIRFunction *fn = &ir->functions[ir->function_count++];
        nova_ir_function_init(fn);
        fn->name = decl->as.fun_decl.name;
        fn->param_count = decl->as.fun_decl.params.count;
        if (fn->param_count > 0) {
            fn->params = calloc(fn->param_count, sizeof(NovaIRParam));
            for (size_t p = 0; p < fn->param_count; ++p) {
                fn->params[p].name = decl->as.fun_decl.params.items[p].name;
                if (decl->as.fun_decl.params.items[p].has_type) {
                    fn->params[p].type = infer_type_from_token(semantics, &decl->as.fun_decl.params.items[p].type_name);
                } else {
                    fn->params[p].type = semantics->type_unknown;
                }
            }
        }
        const NovaExprInfo *body_info = nova_semantic_lookup_expr(semantics, decl->as.fun_decl.body);
        fn->return_type = body_info ? body_info->type : semantics->type_unknown;
        fn->effects = body_info ? body_info->effects : NOVA_EFFECT_NONE;
        fn->body = lower_expr(decl->as.fun_decl.body, semantics);
        if (fn->body) {
            optimize_ir_expr(&fn->body);
        }
    }
    return ir;
}

void nova_ir_free(NovaIRProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->function_count; ++i) {
        nova_ir_function_free(&program->functions[i]);
    }
    free(program->functions);
    free(program);
}
