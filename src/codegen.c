#include "nova/codegen.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void derive_c_path(const char *object_path, char *buffer, size_t size) {
    size_t len = strlen(object_path);
    if (len > 2 && strcmp(object_path + len - 2, ".o") == 0) {
        size_t base_len = len - 2;
        if (base_len + 3 >= size) base_len = size - 4;
        memcpy(buffer, object_path, base_len);
        buffer[base_len] = '\0';
        strncat(buffer, ".c", size - base_len - 1);
    } else {
        snprintf(buffer, size, "%s.c", object_path);
    }
}

static void derive_wrapper_path(const char *output_path, char *buffer, size_t size) {
    snprintf(buffer, size, "%s.wrapper.c", output_path);
}

static void derive_ir_path(const char *output_path, char *buffer, size_t size) {
    snprintf(buffer, size, "%s.ll", output_path);
}

static void emit_token(FILE *out, NovaToken token) {
    fwrite(token.lexeme, 1, token.length, out);
}

static void emit_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; ++i) {
        fputs("    ", out);
    }
}

static const char *type_to_c(const NovaSemanticContext *semantics, NovaTypeId type) {
    const NovaTypeInfo *info = nova_semantic_type_info(semantics, type);
    if (!info) return "double";
    switch (info->kind) {
    case NOVA_TYPE_KIND_NUMBER:
        return "double";
    case NOVA_TYPE_KIND_BOOL:
        return "bool";
    case NOVA_TYPE_KIND_STRING:
        return "const char *";
    case NOVA_TYPE_KIND_UNIT:
        return "void";
    case NOVA_TYPE_KIND_FUNCTION:
    case NOVA_TYPE_KIND_LIST:
    case NOVA_TYPE_KIND_CUSTOM:
    case NOVA_TYPE_KIND_UNKNOWN:
    default:
        return "double";
    }
}

static const char *type_to_llvm(const NovaSemanticContext *semantics, NovaTypeId type) {
    const NovaTypeInfo *info = nova_semantic_type_info(semantics, type);
    if (!info) return "double";
    switch (info->kind) {
    case NOVA_TYPE_KIND_NUMBER:
        return "double";
    case NOVA_TYPE_KIND_BOOL:
        return "i1";
    case NOVA_TYPE_KIND_STRING:
        return "ptr";
    case NOVA_TYPE_KIND_UNIT:
        return "void";
    case NOVA_TYPE_KIND_FUNCTION:
    case NOVA_TYPE_KIND_LIST:
    case NOVA_TYPE_KIND_CUSTOM:
    case NOVA_TYPE_KIND_UNKNOWN:
    default:
        return "double";
    }
}

typedef struct {
    FILE *out;
    const NovaSemanticContext *semantics;
    size_t temp_counter;
    size_t label_counter;
} LLVMEmitter;

static const char *llvm_expr_type(const NovaSemanticContext *semantics, const NovaIRExpr *expr) {
    if (!expr) return "double";
    return type_to_llvm(semantics, expr->type);
}

static void llvm_new_temp(LLVMEmitter *emitter, char *buffer, size_t size) {
    snprintf(buffer, size, "%%t%zu", emitter->temp_counter++);
}

static void llvm_new_label(LLVMEmitter *emitter, char *buffer, size_t size, const char *prefix) {
    snprintf(buffer, size, "%s%zu", prefix, emitter->label_counter++);
}

static void llvm_emitf(LLVMEmitter *emitter, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(emitter->out, fmt, args);
    va_end(args);
}

static bool emit_expr_llvm(LLVMEmitter *emitter, const NovaIRExpr *expr, char *value_buffer, size_t value_buffer_size);

static const char *llvm_zero_literal(const char *type_name) {
    if (strcmp(type_name, "double") == 0) return "0.0";
    if (strcmp(type_name, "i1") == 0) return "0";
    if (strcmp(type_name, "ptr") == 0) return "null";
    return "0";
}

static bool emit_statement_llvm(LLVMEmitter *emitter, const NovaIRExpr *expr) {
    if (!expr) return true;
    if (expr->kind == NOVA_IR_EXPR_SEQUENCE) {
        for (size_t i = 0; i < expr->as.sequence.count; ++i) {
            if (!emit_statement_llvm(emitter, expr->as.sequence.items[i])) return false;
        }
        return true;
    }
    if (expr->kind == NOVA_IR_EXPR_WHILE) {
        char cond_label[32], body_label[32], end_label[32], cond_value[32];
        llvm_new_label(emitter, cond_label, sizeof(cond_label), "while.cond.");
        llvm_new_label(emitter, body_label, sizeof(body_label), "while.body.");
        llvm_new_label(emitter, end_label, sizeof(end_label), "while.end.");
        llvm_emitf(emitter, "  br label %%%s\n", cond_label);
        llvm_emitf(emitter, "%s:\n", cond_label);
        if (!emit_expr_llvm(emitter, expr->as.while_expr.condition, cond_value, sizeof(cond_value))) return false;
        llvm_emitf(emitter, "  br i1 %s, label %%%s, label %%%s\n", cond_value, body_label, end_label);
        llvm_emitf(emitter, "%s:\n", body_label);
        if (!emit_statement_llvm(emitter, expr->as.while_expr.body)) return false;
        llvm_emitf(emitter, "  br label %%%s\n", cond_label);
        llvm_emitf(emitter, "%s:\n", end_label);
        return true;
    }
    char ignored[32];
    return emit_expr_llvm(emitter, expr, ignored, sizeof(ignored));
}

static bool emit_expr_llvm(LLVMEmitter *emitter, const NovaIRExpr *expr, char *value_buffer, size_t value_buffer_size) {
    if (!expr) {
        snprintf(value_buffer, value_buffer_size, "0.0");
        return true;
    }
    switch (expr->kind) {
    case NOVA_IR_EXPR_NUMBER:
        snprintf(value_buffer, value_buffer_size, "%#.17g", expr->as.number_value);
        return true;
    case NOVA_IR_EXPR_BOOL:
        snprintf(value_buffer, value_buffer_size, "%d", expr->as.bool_value ? 1 : 0);
        return true;
    case NOVA_IR_EXPR_UNIT:
        snprintf(value_buffer, value_buffer_size, "0");
        return true;
    case NOVA_IR_EXPR_IDENTIFIER:
        snprintf(value_buffer, value_buffer_size, "%%%.*s", (int)expr->as.identifier.length, expr->as.identifier.lexeme);
        return true;
    case NOVA_IR_EXPR_CALL: {
        const char *ret_type = llvm_expr_type(emitter->semantics, expr);
        char args_buffer[1024] = {0};
        size_t used = 0;
        for (size_t i = 0; i < expr->as.call.arg_count; ++i) {
            char arg_val[64];
            const NovaIRExpr *arg_expr = expr->as.call.args[i];
            if (!emit_expr_llvm(emitter, arg_expr, arg_val, sizeof(arg_val))) return false;
            const char *arg_type = llvm_expr_type(emitter->semantics, arg_expr);
            int written = snprintf(args_buffer + used, sizeof(args_buffer) - used, "%s%s %s", i == 0 ? "" : ", ", arg_type, arg_val);
            if (written < 0 || (size_t)written >= sizeof(args_buffer) - used) return false;
            used += (size_t)written;
        }
        if (strcmp(ret_type, "void") == 0) {
            llvm_emitf(emitter, "  call void @%.*s(%s)\n", (int)expr->as.call.callee.length, expr->as.call.callee.lexeme, args_buffer);
            snprintf(value_buffer, value_buffer_size, "0");
        } else {
            llvm_new_temp(emitter, value_buffer, value_buffer_size);
            llvm_emitf(emitter, "  %s = call %s @%.*s(%s)\n", value_buffer, ret_type, (int)expr->as.call.callee.length, expr->as.call.callee.lexeme, args_buffer);
        }
        return true;
    }
    case NOVA_IR_EXPR_SEQUENCE: {
        if (expr->as.sequence.count == 0) {
            snprintf(value_buffer, value_buffer_size, "0");
            return true;
        }
        for (size_t i = 0; i + 1 < expr->as.sequence.count; ++i) {
            if (!emit_statement_llvm(emitter, expr->as.sequence.items[i])) return false;
        }
        return emit_expr_llvm(emitter, expr->as.sequence.items[expr->as.sequence.count - 1], value_buffer, value_buffer_size);
    }
    case NOVA_IR_EXPR_IF: {
        char cond_value[64];
        if (!emit_expr_llvm(emitter, expr->as.if_expr.condition, cond_value, sizeof(cond_value))) return false;

        char then_label[32], else_label[32], end_label[32];
        llvm_new_label(emitter, then_label, sizeof(then_label), "if.then.");
        llvm_new_label(emitter, else_label, sizeof(else_label), "if.else.");
        llvm_new_label(emitter, end_label, sizeof(end_label), "if.end.");

        llvm_emitf(emitter, "  br i1 %s, label %%%s, label %%%s\n", cond_value, then_label, else_label);
        llvm_emitf(emitter, "%s:\n", then_label);
        char then_value[64];
        if (!emit_expr_llvm(emitter, expr->as.if_expr.then_branch, then_value, sizeof(then_value))) return false;
        llvm_emitf(emitter, "  br label %%%s\n", end_label);

        llvm_emitf(emitter, "%s:\n", else_label);
        char else_value[64];
        if (expr->as.if_expr.else_branch) {
            if (!emit_expr_llvm(emitter, expr->as.if_expr.else_branch, else_value, sizeof(else_value))) return false;
        } else {
            snprintf(else_value, sizeof(else_value), "%s", llvm_zero_literal(llvm_expr_type(emitter->semantics, expr)));
        }
        llvm_emitf(emitter, "  br label %%%s\n", end_label);

        llvm_emitf(emitter, "%s:\n", end_label);
        const char *result_type = llvm_expr_type(emitter->semantics, expr);
        if (strcmp(result_type, "void") == 0) {
            snprintf(value_buffer, value_buffer_size, "0");
            return true;
        }
        llvm_new_temp(emitter, value_buffer, value_buffer_size);
        llvm_emitf(emitter,
                   "  %s = phi %s [ %s, %%%s ], [ %s, %%%s ]\n",
                   value_buffer,
                   result_type,
                   then_value,
                   then_label,
                   else_value,
                   else_label);
        return true;
    }
    case NOVA_IR_EXPR_WHILE:
        return emit_statement_llvm(emitter, expr);
    case NOVA_IR_EXPR_STRING:
    case NOVA_IR_EXPR_LIST:
    case NOVA_IR_EXPR_MATCH:
    default:
        return false;
    }
}

static bool emit_program_llvm(const NovaIRProgram *program, const NovaSemanticContext *semantics, const char *ir_path, char *error_buffer, size_t error_buffer_size) {
    FILE *out = fopen(ir_path, "w");
    if (!out) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "failed to open %s", ir_path);
        }
        return false;
    }
    LLVMEmitter emitter = {.out = out, .semantics = semantics, .temp_counter = 0, .label_counter = 0};
    fputs("target triple = \"x86_64-unknown-linux-gnu\"\n\n", out);
    for (size_t i = 0; i < program->function_count; ++i) {
        const NovaIRFunction *fn = &program->functions[i];
        const char *ret_type = type_to_llvm(semantics, fn->return_type);
        llvm_emitf(&emitter, "define %s @%.*s(", ret_type, (int)fn->name.length, fn->name.lexeme);
        for (size_t p = 0; p < fn->param_count; ++p) {
            if (p > 0) fputs(", ", out);
            llvm_emitf(&emitter, "%s %%%.*s", type_to_llvm(semantics, fn->params[p].type), (int)fn->params[p].name.length, fn->params[p].name.lexeme);
        }
        fputs(") {\nentry:\n", out);
        if (strcmp(ret_type, "void") == 0) {
            if (!emit_statement_llvm(&emitter, fn->body)) {
                if (error_buffer && error_buffer_size > 0) snprintf(error_buffer, error_buffer_size, "unsupported LLVM statement");
                fclose(out);
                remove(ir_path);
                return false;
            }
            fputs("  ret void\n", out);
        } else {
            char result[64];
            if (!emit_expr_llvm(&emitter, fn->body, result, sizeof(result))) {
                if (error_buffer && error_buffer_size > 0) snprintf(error_buffer, error_buffer_size, "unsupported LLVM expression");
                fclose(out);
                remove(ir_path);
                return false;
            }
            llvm_emitf(&emitter, "  ret %s %s\n", ret_type, result);
        }
        fputs("}\n\n", out);
    }
    fclose(out);
    return true;
}

static bool emit_expr(FILE *out, const NovaSemanticContext *semantics, const NovaIRExpr *expr);

static bool emit_statement(FILE *out, const NovaSemanticContext *semantics, const NovaIRExpr *expr, int indent) {
    if (!expr) {
        emit_indent(out, indent);
        fputs(";", out);
        return true;
    }
    if (expr->kind == NOVA_IR_EXPR_SEQUENCE) {
        for (size_t i = 0; i < expr->as.sequence.count; ++i) {
            if (!emit_statement(out, semantics, expr->as.sequence.items[i], indent)) return false;
            if (i + 1 < expr->as.sequence.count) {
                fputc('\n', out);
            }
        }
        return true;
    }
    if (expr->kind == NOVA_IR_EXPR_WHILE) {
        emit_indent(out, indent);
        fputs("while (", out);
        if (!emit_expr(out, semantics, expr->as.while_expr.condition)) return false;
        fputs(") {\n", out);
        if (!emit_statement(out, semantics, expr->as.while_expr.body, indent + 1)) return false;
        fputc('\n', out);
        emit_indent(out, indent);
        fputs("}", out);
        return true;
    }
    emit_indent(out, indent);
    if (!emit_expr(out, semantics, expr)) return false;
    fputs(";", out);
    return true;
}

static bool emit_expr(FILE *out, const NovaSemanticContext *semantics, const NovaIRExpr *expr) {
    if (!expr) {
        fputs("0", out);
        return true;
    }
    switch (expr->kind) {
    case NOVA_IR_EXPR_NUMBER:
        fprintf(out, "%g", expr->as.number_value);
        return true;
    case NOVA_IR_EXPR_BOOL:
        fputs(expr->as.bool_value ? "true" : "false", out);
        return true;
    case NOVA_IR_EXPR_STRING:
        if (expr->as.string_value.text) {
            fputs(expr->as.string_value.text, out);
        } else {
            fputs("\"\"", out);
        }
        return true;
    case NOVA_IR_EXPR_UNIT:
        fputs("0", out);
        return true;
    case NOVA_IR_EXPR_IDENTIFIER:
        emit_token(out, expr->as.identifier);
        return true;
    case NOVA_IR_EXPR_CALL:
        emit_token(out, expr->as.call.callee);
        fputc('(', out);
        for (size_t i = 0; i < expr->as.call.arg_count; ++i) {
            if (i > 0) fputs(", ", out);
            if (!emit_expr(out, semantics, expr->as.call.args[i])) return false;
        }
        fputc(')', out);
        return true;
    case NOVA_IR_EXPR_SEQUENCE:
        if (expr->as.sequence.count == 0) {
            fputs("0", out);
            return true;
        }
        if (expr->as.sequence.count == 1) {
            return emit_expr(out, semantics, expr->as.sequence.items[0]);
        }
        fputc('(', out);
        for (size_t i = 0; i < expr->as.sequence.count; ++i) {
            if (i > 0) fputs(", ", out);
            if (!emit_expr(out, semantics, expr->as.sequence.items[i])) return false;
        }
        fputc(')', out);
        return true;
    case NOVA_IR_EXPR_IF: {
        NovaIRExpr *cond = expr->as.if_expr.condition;
        if (cond && cond->kind == NOVA_IR_EXPR_BOOL) {
            if (cond->as.bool_value) {
                return emit_expr(out, semantics, expr->as.if_expr.then_branch);
            }
            if (expr->as.if_expr.else_branch) {
                return emit_expr(out, semantics, expr->as.if_expr.else_branch);
            }
            fputs("0", out);
            return true;
        }
        fputc('(', out);
        if (!emit_expr(out, semantics, expr->as.if_expr.condition)) return false;
        fputs(" ? ", out);
        if (!emit_expr(out, semantics, expr->as.if_expr.then_branch)) return false;
        fputs(" : ", out);
        if (expr->as.if_expr.else_branch) {
            if (!emit_expr(out, semantics, expr->as.if_expr.else_branch)) return false;
        } else {
            fputs("0", out);
        }
        fputc(')', out);
        return true;
    }
    case NOVA_IR_EXPR_WHILE:
    case NOVA_IR_EXPR_LIST:
    case NOVA_IR_EXPR_MATCH:
        return false;
    default:
        return false;
    }
}

static bool emit_function(FILE *out, const NovaSemanticContext *semantics, const NovaIRFunction *fn) {
    const char *return_type = type_to_c(semantics, fn->return_type);
    fprintf(out, "%s ", return_type);
    emit_token(out, fn->name);
    fputc('(', out);
    if (fn->param_count == 0) {
        fputs("void", out);
    } else {
        for (size_t i = 0; i < fn->param_count; ++i) {
            if (i > 0) fputs(", ", out);
            const char *param_type = type_to_c(semantics, fn->params[i].type);
            fprintf(out, "%s ", param_type);
            emit_token(out, fn->params[i].name);
        }
    }
    fputs(") {\n", out);
    if (strcmp(return_type, "void") != 0) {
        emit_indent(out, 1);
        fputs("return ", out);
        if (!emit_expr(out, semantics, fn->body)) return false;
        fputs(";\n", out);
    } else if (fn->body) {
        if (!emit_statement(out, semantics, fn->body, 1)) return false;
        fputc('\n', out);
    }
    fputs("}\n\n", out);
    return true;
}

static bool emit_program_c(const NovaIRProgram *program, const NovaSemanticContext *semantics, const char *c_path, char *error_buffer, size_t error_buffer_size) {
    FILE *out = fopen(c_path, "w");
    if (!out) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "failed to open %s", c_path);
        }
        return false;
    }
    fputs("#include <stdbool.h>\n\n", out);
    for (size_t i = 0; i < program->function_count; ++i) {
        if (!emit_function(out, semantics, &program->functions[i])) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "unsupported expression in function");
            }
            fclose(out);
            remove(c_path);
            return false;
        }
    }
    fclose(out);
    return true;
}

static int invoke_cc(const char *source_path, const char *output_path, bool link_executable) {
    const char *cc = getenv("NOVA_CC");
    if (!cc || cc[0] == '\0') {
        cc = "cc";
    }
    const char *common_flags = "-std=c11 -O3 -flto -fno-plt -fomit-frame-pointer -DNDEBUG";
    char command[PATH_MAX * 4];
    if (link_executable) {
        snprintf(command,
                 sizeof(command),
                 "%s %s -Wl,--gc-sections %s -o %s",
                 cc,
                 common_flags,
                 source_path,
                 output_path);
    } else {
        snprintf(command,
                 sizeof(command),
                 "%s %s -c %s -o %s",
                 cc,
                 common_flags,
                 source_path,
                 output_path);
    }
    return system(command);
}

static int invoke_llvm_cc(const char *ir_path, const char *output_path, bool link_executable) {
    const char *cc = getenv("NOVA_CC");
    if (!cc || cc[0] == '\0') {
        cc = "clang";
    }
    const char *common_flags = "-O3 -ffast-math -funroll-loops -fvectorize -fslp-vectorize -fno-plt -fomit-frame-pointer -DNDEBUG";
    char command[PATH_MAX * 4];
    if (link_executable) {
        snprintf(command, sizeof(command), "%s %s -x ir %s -Wl,--gc-sections -o %s", cc, common_flags, ir_path, output_path);
    } else {
        snprintf(command, sizeof(command), "%s %s -c -x ir %s -o %s", cc, common_flags, ir_path, output_path);
    }
    return system(command);
}

static bool use_llvm_backend(void) {
    const char *backend = getenv("NOVA_CODEGEN_BACKEND");
    return backend && strcmp(backend, "llvm") == 0;
}

bool nova_codegen_emit_object(const NovaIRProgram *program, const NovaSemanticContext *semantics, const char *object_path, char *error_buffer, size_t error_buffer_size) {
    if (!program || !object_path) {
        return false;
    }

    if (use_llvm_backend()) {
        char ir_path[PATH_MAX];
        derive_ir_path(object_path, ir_path, sizeof(ir_path));
        if (!emit_program_llvm(program, semantics, ir_path, error_buffer, error_buffer_size)) {
            return false;
        }
        int result = invoke_llvm_cc(ir_path, object_path, false);
        remove(ir_path);
        if (result != 0) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "LLVM code generation failed (clang exit %d)", result);
            }
            return false;
        }
        return true;
    }

    char c_path[PATH_MAX];
    derive_c_path(object_path, c_path, sizeof(c_path));
    if (!emit_program_c(program, semantics, c_path, error_buffer, error_buffer_size)) {
        return false;
    }

    int result = invoke_cc(c_path, object_path, false);
    if (result != 0) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "code generation failed (cc exit %d)", result);
        }
        remove(c_path);
        return false;
    }
    remove(c_path);
    return true;
}

bool nova_codegen_emit_executable(const NovaIRProgram *program, const NovaSemanticContext *semantics, const char *executable_path, const char *entry_function, char *error_buffer, size_t error_buffer_size) {
    if (!program || !executable_path || !entry_function || entry_function[0] == '\0') {
        return false;
    }

    if (use_llvm_backend()) {
        char ir_path[PATH_MAX];
        derive_ir_path(executable_path, ir_path, sizeof(ir_path));
        if (!emit_program_llvm(program, semantics, ir_path, error_buffer, error_buffer_size)) {
            return false;
        }
        FILE *out = fopen(ir_path, "a");
        if (!out) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "failed to update %s", ir_path);
            }
            remove(ir_path);
            return false;
        }
        fprintf(out,
                "define i32 @main() {\nentry:\n  %%result = call double @%s()\n  %%int = fptosi double %%result to i32\n  ret i32 %%int\n}\n",
                entry_function);
        fclose(out);

        int result = invoke_llvm_cc(ir_path, executable_path, true);
        remove(ir_path);
        if (result != 0) {
            if (error_buffer && error_buffer_size > 0) {
                snprintf(error_buffer, error_buffer_size, "LLVM AOT executable generation failed (clang exit %d)", result);
            }
            return false;
        }
        return true;
    }

    char c_path[PATH_MAX];
    derive_c_path(executable_path, c_path, sizeof(c_path));
    if (!emit_program_c(program, semantics, c_path, error_buffer, error_buffer_size)) {
        return false;
    }

    char wrapper_path[PATH_MAX];
    derive_wrapper_path(executable_path, wrapper_path, sizeof(wrapper_path));
    FILE *wrapper = fopen(wrapper_path, "w");
    if (!wrapper) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "failed to open %s", wrapper_path);
        }
        remove(c_path);
        return false;
    }

    fprintf(wrapper,
            "double %s(void);\n"
            "int main(void) {\n"
            "    return (int)%s();\n"
            "}\n",
            entry_function,
            entry_function);
    fclose(wrapper);

    char merge_path[PATH_MAX];
    snprintf(merge_path, sizeof(merge_path), "%s.merge.c", executable_path);
    FILE *merged = fopen(merge_path, "w");
    if (!merged) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "failed to open %s", merge_path);
        }
        remove(c_path);
        remove(wrapper_path);
        return false;
    }

    FILE *source = fopen(c_path, "r");
    FILE *entry = fopen(wrapper_path, "r");
    if (!source || !entry) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "failed to read generated sources");
        }
        if (source) fclose(source);
        if (entry) fclose(entry);
        fclose(merged);
        remove(c_path);
        remove(wrapper_path);
        remove(merge_path);
        return false;
    }

    int ch;
    while ((ch = fgetc(source)) != EOF) {
        fputc(ch, merged);
    }
    fputc('\n', merged);
    while ((ch = fgetc(entry)) != EOF) {
        fputc(ch, merged);
    }
    fclose(source);
    fclose(entry);
    fclose(merged);

    int result = invoke_cc(merge_path, executable_path, true);
    remove(c_path);
    remove(wrapper_path);
    remove(merge_path);
    if (result != 0) {
        if (error_buffer && error_buffer_size > 0) {
            snprintf(error_buffer, error_buffer_size, "AOT executable generation failed (cc exit %d)", result);
        }
        return false;
    }

    return true;
}
