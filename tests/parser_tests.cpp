#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "nova/codegen.h"
#include "nova/ir.h"
#include "nova/parser.h"
#include "nova/semantic.h"

static const char *CORE_PROGRAM =
    "module demo.core\n"
    "type Option = Some(Number) | None\n"
    "fun identity(x: Number): Number = x\n"
    "fun wrap(): Option = Some(42)\n"
    "fun choose(v: Option): Number = match v { Some(value) -> value; None -> 0 }\n"
    "fun pipeline(): Number = 1 |> identity\n"
    "fun later(): Number = async { 42 }\n";

static bool token_matches(const NovaToken *token, const char *text) {
    size_t len = strlen(text);
    return token->length == len && strncmp(token->lexeme, text, len) == 0;
}

static const NovaIRFunction *find_function(const NovaIRProgram *ir, const char *name) {
    if (!ir || !name) return NULL;
    for (size_t i = 0; i < ir->function_count; ++i) {
        if (token_matches(&ir->functions[i].name, name)) {
            return &ir->functions[i];
        }
    }
    return NULL;
}

static char *read_file_contents(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }
    buffer[size] = '\0';
    return buffer;
}

static bool write_file_contents(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    size_t len = strlen(contents);
    bool ok = fwrite(contents, 1, len, file) == len;
    fclose(file);
    return ok;
}

static int nova_mkdir(const char *path, int mode) {
#ifdef _WIN32
    (void)mode;
    if (_mkdir(path) == 0) {
        return 0;
    }
    return -1;
#else
    return mkdir(path, mode);
#endif
}

static long nova_process_id(void) {
#ifdef _WIN32
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static char *make_temp_dir(char *dir_template) {
    size_t len = strlen(dir_template);
    char *x_start = strchr(dir_template, 'X');
    if (!x_start) {
        errno = EINVAL;
        return NULL;
    }
    size_t x_count = len - (size_t)(x_start - dir_template);
    static unsigned long counter = 0;
    for (int attempt = 0; attempt < 4096; ++attempt) {
        unsigned long value = (unsigned long)nova_process_id() + counter + (unsigned long)attempt;
        for (size_t i = 0; i < x_count; ++i) {
            unsigned digit = (unsigned)(value % 36);
            value /= 36;
            x_start[i] = (digit < 10) ? (char)('0' + digit) : (char)('a' + (digit - 10));
        }
        if (nova_mkdir(dir_template, 0700) == 0) {
            counter += (unsigned long)(attempt + 1);
            return dir_template;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
}

static void test_parser_and_semantics(void) {
    NovaParser parser;
    nova_parser_init(&parser, CORE_PROGRAM);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);
    assert(program->decl_count == 6);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count == 0);

    // ensure pipeline function infers Number type without effects
    const NovaFunDecl *pipeline = &program->decls[4].as.fun_decl;
    const NovaExprInfo *pipeline_info = nova_semantic_lookup_expr(&ctx, pipeline->body);
    assert(pipeline_info != NULL);
    assert(pipeline_info->type == ctx.type_number);
    assert((pipeline_info->effects & NOVA_EFFECT_ASYNC) == 0);

    // ensure async function records async effect
    const NovaFunDecl *later = &program->decls[5].as.fun_decl;
    const NovaExprInfo *later_info = nova_semantic_lookup_expr(&ctx, later->body);
    assert(later_info != NULL);
    assert((later_info->effects & NOVA_EFFECT_ASYNC) != 0);

    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_match_exhaustiveness_warning(void) {
    const char *source =
        "module demo.flags\n"
        "type Flag = Yes | No\n"
        "fun only_yes(f: Flag): Number = match f { Yes -> 1 }\n";

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count > 0);

    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_codegen_pipeline(void) {
    const char *source =
        "module demo.codegen\n"
        "fun main(): Number = if true { 42 } else { 0 }\n";

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count == 0);

    NovaIRProgram *ir = nova_ir_lower(program, &ctx);
    assert(ir != NULL);

    const char *object_path = "build/main.o";
    char error[256];
    bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
    assert(ok && "code generation failed");

    struct stat st;
    assert(stat(object_path, &st) == 0);

    remove(object_path);
    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_ir_lowering_extensions(void) {
    const char *source =
        "module demo.ir\n"
        "fun identity(x: Number): Number = x\n"
        "fun double(x: Number): Number = x\n"
        "fun compute(): Number = 1 |> identity |> double\n"
        "fun conditional(flag: Bool): Number = if flag { 1 } else { 0 }\n";

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);

    NovaIRProgram *ir = nova_ir_lower(program, &ctx);
    assert(ir != NULL);

    const NovaIRFunction *compute_fn = NULL;
    const NovaIRFunction *conditional_fn = NULL;
    for (size_t i = 0; i < ir->function_count; ++i) {
        if (token_matches(&ir->functions[i].name, "compute")) {
            compute_fn = &ir->functions[i];
        } else if (token_matches(&ir->functions[i].name, "conditional")) {
            conditional_fn = &ir->functions[i];
        }
    }
    assert(compute_fn != NULL);
    assert(conditional_fn != NULL);

    assert(compute_fn->body != NULL);
    assert(compute_fn->body->kind == NOVA_IR_EXPR_CALL);
    assert(token_matches(&compute_fn->body->as.call.callee, "double"));
    assert(compute_fn->body->as.call.arg_count == 1);
    const NovaIRExpr *inner_call = compute_fn->body->as.call.args[0];
    assert(inner_call != NULL);
    assert(inner_call->kind == NOVA_IR_EXPR_CALL);
    assert(token_matches(&inner_call->as.call.callee, "identity"));
    assert(inner_call->as.call.arg_count == 1);
    const NovaIRExpr *literal = inner_call->as.call.args[0];
    assert(literal != NULL && literal->kind == NOVA_IR_EXPR_NUMBER);

    assert(conditional_fn->body != NULL);
    assert(conditional_fn->body->kind == NOVA_IR_EXPR_IF);
    assert(conditional_fn->body->as.if_expr.condition->kind == NOVA_IR_EXPR_IDENTIFIER);
    assert(conditional_fn->body->as.if_expr.then_branch->kind == NOVA_IR_EXPR_NUMBER);
    assert(conditional_fn->body->as.if_expr.else_branch->kind == NOVA_IR_EXPR_NUMBER);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_ir_control_flow_optimizations(void) {
    const char *source =
        "module demo.optimize\n"
        "fun helper(): Number = 5\n"
        "fun prefer(): Number = if true { helper() } else { 0 }\n"
        "fun fallback(): Number = if false { 1 } else { 2 }\n";

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);

    NovaIRProgram *ir = nova_ir_lower(program, &ctx);
    assert(ir != NULL);

    const NovaIRFunction *prefer_fn = find_function(ir, "prefer");
    assert(prefer_fn != NULL);
    assert(prefer_fn->body != NULL);
    assert(prefer_fn->body->kind == NOVA_IR_EXPR_CALL);
    assert(token_matches(&prefer_fn->body->as.call.callee, "helper"));

    const NovaIRFunction *fallback_fn = find_function(ir, "fallback");
    assert(fallback_fn != NULL);
    assert(fallback_fn->body != NULL);
    assert(fallback_fn->body->kind == NOVA_IR_EXPR_NUMBER);
    assert(fallback_fn->body->as.number_value == 2.0);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_project_generator(void) {
    char dir_template[] = "build/nova_projXXXXXX";
    char *project_dir = make_temp_dir(dir_template);
    assert(project_dir != NULL);

    char command[PATH_MAX * 2];
    snprintf(command, sizeof(command), "./build/nova-new %s", project_dir);
    int rc = system(command);
    assert(rc == 0);

    struct stat st;
    char toml_path[PATH_MAX];
    snprintf(toml_path, sizeof(toml_path), "%s/nova.toml", project_dir);
    assert(stat(toml_path, &st) == 0);

    char main_path[PATH_MAX];
    snprintf(main_path, sizeof(main_path), "%s/src/main.nova", project_dir);
    assert(stat(main_path, &st) == 0);

    char *source = read_file_contents(main_path);
    assert(source != NULL);

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count == 0);

    NovaIRProgram *ir = nova_ir_lower(program, &ctx);
    assert(ir != NULL);

    const NovaIRFunction *main_fn = find_function(ir, "main");
    assert(main_fn != NULL);
    assert(main_fn->body != NULL);
    assert(main_fn->body->kind == NOVA_IR_EXPR_CALL);
    assert(token_matches(&main_fn->body->as.call.callee, "answer"));

    char object_path[PATH_MAX];
    snprintf(object_path, sizeof(object_path), "%s/main.o", project_dir);
    char error[256];
    bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
    assert(ok && "code generation failed for generated project");
    assert(stat(object_path, &st) == 0);
    remove(object_path);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
    free(source);

    char cleanup_cmd[PATH_MAX * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", project_dir);
    system(cleanup_cmd);
}

static void test_while_loop_codegen(void) {
    const char *source =
        "module demo.loop\n"
        "fun spin(flag: Bool): Unit = while flag { 1 }\n";

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count == 0);

    const NovaFunDecl *spin = &program->decls[0].as.fun_decl;
    const NovaExprInfo *spin_info = nova_semantic_lookup_expr(&ctx, spin->body);
    assert(spin_info != NULL);
    assert(spin_info->type == ctx.type_unit);

    NovaIRProgram *ir = nova_ir_lower(program, &ctx);
    assert(ir != NULL);

    const NovaIRFunction *spin_fn = find_function(ir, "spin");
    assert(spin_fn != NULL);
    assert(spin_fn->body != NULL);
    assert(spin_fn->body->kind == NOVA_IR_EXPR_WHILE);

    const char *object_path = "build/spin.o";
    char error[256];
    bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
    assert(ok && "code generation failed for while loop");

    struct stat st;
    assert(stat(object_path, &st) == 0);

    remove(object_path);
    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_stability_checker_cli(void) {
    char dir_template[] = "build/nova_checkXXXXXX";
    char *check_dir = make_temp_dir(dir_template);
    assert(check_dir != NULL);

    char source_path[PATH_MAX];
    snprintf(source_path, sizeof(source_path), "%s/check.nova", check_dir);
    const char *source =
        "module demo.check\n"
        "fun counter(flag: Bool): Unit = while flag { 1 }\n";
    assert(write_file_contents(source_path, source));

    char command[PATH_MAX * 2];
    snprintf(command, sizeof(command), "./build/nova-check %s", source_path);
    int rc = system(command);
    assert(rc == 0);

    char cleanup_cmd[PATH_MAX * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", check_dir);
    system(cleanup_cmd);
}

static void test_examples(void) {
    struct ExampleCheck {
        const char *path;
        const char *flags;
    };
    const struct ExampleCheck examples[] = {
        { "examples/pipeline.nova", "" },
        { "examples/options.nova", "--skip-codegen " },
        { "examples/loop.nova", "" },
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        char command[PATH_MAX * 2];
        snprintf(command, sizeof(command), "./build/nova-check %s%s", examples[i].flags, examples[i].path);
        int rc = system(command);
        assert(rc == 0);
    }
}

int main(void) {
    test_parser_and_semantics();
    test_match_exhaustiveness_warning();
    test_codegen_pipeline();
    test_ir_lowering_extensions();
    test_ir_control_flow_optimizations();
    test_while_loop_codegen();
    test_project_generator();
    test_stability_checker_cli();
    test_examples();
    printf("All tests passed.\n");
    return 0;
}
