#include "nova/codegen.h"

#include <limits.h>
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

bool nova_codegen_emit_object(const NovaIRProgram *program, const NovaSemanticContext *semantics, const char *object_path, char *error_buffer, size_t error_buffer_size) {
    if (!program || !object_path) {
        return false;
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
