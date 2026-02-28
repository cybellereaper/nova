#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <time.h>

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
#include "nova/gc.h"
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

static char *make_temp_dir(char *template) {
    size_t len = strlen(template);
    char *x_start = strchr(template, 'X');
    if (!x_start) {
        errno = EINVAL;
        return NULL;
    }
    size_t x_count = len - (size_t)(x_start - template);
    static unsigned long counter = 0;
    for (int attempt = 0; attempt < 4096; ++attempt) {
        unsigned long value = (unsigned long)nova_process_id() + counter + (unsigned long)attempt;
        for (size_t i = 0; i < x_count; ++i) {
            unsigned digit = (unsigned)(value % 36);
            value /= 36;
            x_start[i] = (digit < 10) ? (char)('0' + digit) : (char)('a' + (digit - 10));
        }
        if (nova_mkdir(template, 0700) == 0) {
            counter += (unsigned long)(attempt + 1);
            return template;
        }
        if (errno != EEXIST) {
            return NULL;
        }
    }
    errno = EEXIST;
    return NULL;
}


static char *build_mock_stress_program(size_t function_count, size_t pipeline_depth) {
    size_t estimated = 64 + function_count * (pipeline_depth * 20 + 80);
    char *source = malloc(estimated);
    if (!source) {
        return NULL;
    }
    size_t used = (size_t)snprintf(source, estimated, "module demo.stress\nfun id(x: Number): Number = x\n");
    for (size_t i = 0; i < function_count; ++i) {
        used += (size_t)snprintf(source + used, estimated - used, "fun run_%zu(): Number = %zu", i, i + 1);
        for (size_t stage = 0; stage < pipeline_depth; ++stage) {
            used += (size_t)snprintf(source + used, estimated - used, " |> id");
        }
        used += (size_t)snprintf(source + used, estimated - used, "\n");
    }
    return source;
}


typedef struct {
    int value;
    void *next;
} MockGCNode;

typedef struct {
    size_t alloc_count;
    size_t free_count;
    size_t fail_after;
} MockAllocatorState;

static void *mock_gc_malloc(size_t size, void *user_data) {
    MockAllocatorState *state = (MockAllocatorState *)user_data;
    state->alloc_count++;
    if (state->fail_after > 0 && state->alloc_count > state->fail_after) {
        return NULL;
    }
    return malloc(size);
}

static void mock_gc_free(void *ptr, void *user_data) {
    MockAllocatorState *state = (MockAllocatorState *)user_data;
    state->free_count++;
    free(ptr);
}

static void trace_mock_node(NovaGC *gc, void *object, void *ctx) {
    (void)ctx;
    MockGCNode *node = (MockGCNode *)object;
    nova_gc_mark(gc, node->next);
}

static void test_gc_collects_unreachable_graph(void) {
    NovaGC gc;
    nova_gc_init(&gc, NULL);

    MockGCNode *root = (MockGCNode *)nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL);
    MockGCNode *child = (MockGCNode *)nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL);
    MockGCNode *orphan = (MockGCNode *)nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL);
    assert(root != NULL && child != NULL && orphan != NULL);

    root->value = 1;
    child->value = 2;
    orphan->value = 3;
    root->next = child;

    void *root_slot = root;
    nova_gc_add_root(&gc, &root_slot);
    size_t swept = nova_gc_collect(&gc, NULL);
    assert(swept == 1);

    NovaGCStats stats;
    nova_gc_stats(&gc, &stats);
    assert(stats.object_count == 2);
    assert(stats.collections >= 1);

    root_slot = NULL;
    swept = nova_gc_collect(&gc, NULL);
    assert(swept == 2);

    nova_gc_remove_root(&gc, &root_slot);
    nova_gc_destroy(&gc);
}

static void test_gc_sweep_budget_limits_latency(void) {
    NovaGC gc;
    nova_gc_init(&gc, NULL);

    for (size_t i = 0; i < 10; ++i) {
        assert(nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL) != NULL);
    }

    NovaGCCollectOptions options = { .sweep_budget = 3 };
    size_t swept = nova_gc_collect(&gc, &options);
    assert(swept == 3);

    NovaGCStats stats;
    nova_gc_stats(&gc, &stats);
    assert(stats.object_count == 7);

    options.sweep_budget = 0;
    swept = nova_gc_collect(&gc, &options);
    assert(swept == 7);

    nova_gc_destroy(&gc);
}

static void test_gc_with_mock_allocator(void) {
    MockAllocatorState state = {0};
    state.fail_after = 2;
    NovaGCAllocator allocator = {
        .malloc_fn = mock_gc_malloc,
        .free_fn = mock_gc_free,
        .user_data = &state,
    };

    NovaGC gc;
    nova_gc_init(&gc, &allocator);

    void *root = nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL);
    assert(root != NULL);

    void *slot = root;
    nova_gc_add_root(&gc, &slot);

    void *will_fail = nova_gc_alloc(&gc, sizeof(MockGCNode), trace_mock_node, NULL);
    assert(will_fail == NULL);

    slot = NULL;
    nova_gc_collect(&gc, NULL);
    nova_gc_remove_root(&gc, &slot);
    nova_gc_destroy(&gc);

    assert(state.alloc_count >= 3);
    assert(state.free_count >= 1);
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
    char template[] = "build/nova_projXXXXXX";
    char *project_dir = make_temp_dir(template);
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
    char template[] = "build/nova_checkXXXXXX";
    char *check_dir = make_temp_dir(template);
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


static void test_mocked_semantic_stability_workload(void) {
    char *source = build_mock_stress_program(120, 24);
    assert(source != NULL);

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    assert(program != NULL);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    assert(ctx.diagnostics.count == 0);

    for (size_t i = 1; i < program->decl_count; ++i) {
        const NovaFunDecl *fun = &program->decls[i].as.fun_decl;
        for (size_t repeat = 0; repeat < 40; ++repeat) {
            const NovaExprInfo *info = nova_semantic_lookup_expr(&ctx, fun->body);
            assert(info != NULL);
            assert(info->type == ctx.type_number);
        }
    }

    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
    free(source);
}

static void test_performance_regression_smoke(void) {
    char *source = build_mock_stress_program(220, 18);
    assert(source != NULL);

    clock_t start = clock();
    for (size_t run = 0; run < 8; ++run) {
        NovaParser parser;
        nova_parser_init(&parser, source);
        NovaProgram *program = nova_parser_parse(&parser);
        assert(program != NULL);

        NovaSemanticContext ctx;
        nova_semantic_context_init(&ctx);
        nova_semantic_analyze_program(&ctx, program);
        assert(ctx.diagnostics.count == 0);

        const NovaFunDecl *last_fun = &program->decls[program->decl_count - 1].as.fun_decl;
        const NovaExprInfo *info = nova_semantic_lookup_expr(&ctx, last_fun->body);
        assert(info != NULL);

        nova_semantic_context_free(&ctx);
        nova_program_free(program);
        free(program);
        nova_parser_free(&parser);
    }
    clock_t end = clock();
    double elapsed_ms = ((double)(end - start) * 1000.0) / (double)CLOCKS_PER_SEC;
    printf("performance smoke: %.2fms\n", elapsed_ms);

    free(source);
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
    test_gc_collects_unreachable_graph();
    test_gc_sweep_budget_limits_latency();
    test_gc_with_mock_allocator();
    test_parser_and_semantics();
    test_match_exhaustiveness_warning();
    test_codegen_pipeline();
    test_ir_lowering_extensions();
    test_ir_control_flow_optimizations();
    test_while_loop_codegen();
    test_project_generator();
    test_stability_checker_cli();
    test_mocked_semantic_stability_workload();
    test_performance_regression_smoke();
    test_examples();
    printf("All tests passed.\n");
    return 0;
}
