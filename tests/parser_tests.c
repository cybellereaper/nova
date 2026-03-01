#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "nova/codegen.h"
#include "nova/ir.h"
#include "nova/lexer.h"
#include "nova/parser.h"
#include "nova/semantic.h"
#include "nova/gc.h"

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



static void nova_setenv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
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
    void *child;
    int value;
} MockNode;

typedef struct {
    size_t alloc_calls;
    size_t free_calls;
    size_t fail_after;
} MockAllocator;

static void *mock_alloc(void *ctx, size_t size) {
    MockAllocator *allocator = (MockAllocator *)ctx;
    allocator->alloc_calls += 1;
    if (allocator->fail_after > 0 && allocator->alloc_calls > allocator->fail_after) {
        return NULL;
    }
    return malloc(size);
}

static void mock_free(void *ctx, void *ptr) {
    MockAllocator *allocator = (MockAllocator *)ctx;
    allocator->free_calls += 1;
    free(ptr);
}

static void mock_node_trace(NovaGC *gc, void *payload) {
    MockNode *node = (MockNode *)payload;
    nova_gc_mark_ptr(gc, node->child);
}

static void test_gc_preserves_reachable_objects(void) {
    NovaGC *gc = nova_gc_create(NULL);
    assert(gc != NULL);

    MockNode *root = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
    MockNode *child = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
    MockNode *garbage = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
    assert(root != NULL && child != NULL && garbage != NULL);

    root->child = child;
    root->value = 7;
    child->child = NULL;
    child->value = 42;
    garbage->child = NULL;

    void *root_slot = root;
    assert(nova_gc_add_root(gc, &root_slot));
    nova_gc_collect(gc);

    NovaGCStats stats = nova_gc_stats(gc);
    assert(stats.objects_total == 2);
    assert(stats.objects_swept >= 1);
    assert(stats.bytes_live >= sizeof(MockNode) * 2);

    nova_gc_remove_root(gc, &root_slot);
    root_slot = NULL;
    nova_gc_collect(gc);

    stats = nova_gc_stats(gc);
    assert(stats.objects_total == 0);
    nova_gc_destroy(gc);
}

static void test_gc_incremental_steps(void) {
    NovaGCConfig config = {
        .initial_threshold_bytes = 64,
        .growth_percent = 100,
    };
    NovaGC *gc = nova_gc_create(&config);
    assert(gc != NULL);

    void *root = NULL;
    assert(nova_gc_add_root(gc, &root));

    for (size_t i = 0; i < 200; ++i) {
        MockNode *node = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
        assert(node != NULL);
        node->child = root;
        root = node;
    }

    size_t collections_before = nova_gc_stats(gc).collections;
    nova_gc_collect_step(gc, 8);
    NovaGCStats stepped = nova_gc_stats(gc);
    assert(stepped.collections >= collections_before + 1);
    assert(stepped.collection_in_progress);

    while (nova_gc_stats(gc).collection_in_progress) {
        nova_gc_collect_step(gc, 8);
    }

    root = NULL;
    nova_gc_collect(gc);
    assert(nova_gc_stats(gc).objects_total == 0);

    nova_gc_destroy(gc);
}

static void test_gc_mock_allocator_and_failure(void) {
    MockAllocator allocator = {0};
    NovaGCConfig config = {
        .alloc = mock_alloc,
        .free = mock_free,
        .ctx = &allocator,
        .initial_threshold_bytes = 32,
    };

    NovaGC *gc = nova_gc_create(&config);
    assert(gc != NULL);

    for (size_t i = 0; i < 16; ++i) {
        MockNode *node = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
        assert(node != NULL);
        node->child = NULL;
    }

    nova_gc_collect(gc);
    assert(allocator.alloc_calls > 0);

    allocator.fail_after = allocator.alloc_calls;
    void *maybe_null = nova_gc_alloc(gc, sizeof(MockNode), mock_node_trace, NULL);
    assert(maybe_null == NULL);

    nova_gc_destroy(gc);
    assert(allocator.free_calls > 0);
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

static void test_lexer_keyword_classification(void) {
    const char *source =
        "module import fun let type if while else match async await true false value modulex async_task";
    NovaTokenArray tokens = nova_lexer_tokenize(source);
    assert(tokens.size >= 17);

    const NovaTokenType expected[] = {
        NOVA_TOKEN_MODULE,
        NOVA_TOKEN_IMPORT,
        NOVA_TOKEN_FUN,
        NOVA_TOKEN_LET,
        NOVA_TOKEN_TYPE,
        NOVA_TOKEN_IF,
        NOVA_TOKEN_WHILE,
        NOVA_TOKEN_ELSE,
        NOVA_TOKEN_MATCH,
        NOVA_TOKEN_ASYNC,
        NOVA_TOKEN_AWAIT,
        NOVA_TOKEN_TRUE,
        NOVA_TOKEN_FALSE,
        NOVA_TOKEN_IDENTIFIER,
        NOVA_TOKEN_IDENTIFIER,
        NOVA_TOKEN_IDENTIFIER,
        NOVA_TOKEN_EOF,
    };
    const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    assert(tokens.size == expected_count);
    for (size_t i = 0; i < expected_count; ++i) {
        assert(tokens.data[i].type == expected[i]);
    }

    nova_token_array_free(&tokens);
}

static void test_lexer_large_input_tokenization(void) {
    const size_t statement_count = 5000;
    const size_t estimated = statement_count * 32 + 64;
    char *source = malloc(estimated);
    assert(source != NULL);

    size_t used = (size_t)snprintf(source, estimated, "module stress.tokens\n");
    for (size_t i = 0; i < statement_count; ++i) {
        used += (size_t)snprintf(source + used, estimated - used, "let v%zu = %zu\n", i, i);
    }

    NovaTokenArray tokens = nova_lexer_tokenize(source);
    assert(tokens.size > statement_count * 4);
    assert(tokens.data[tokens.size - 1].type == NOVA_TOKEN_EOF);
    assert(tokens.data[0].type == NOVA_TOKEN_MODULE);
    assert(tokens.capacity >= tokens.size);

    nova_token_array_free(&tokens);
    free(source);
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



static void test_codegen_uses_low_latency_flags(void) {
    char template[] = "build/nova_ccXXXXXX";
    char *dir = make_temp_dir(template);
    assert(dir != NULL);

    char cc_path[PATH_MAX];
    snprintf(cc_path, sizeof(cc_path), "%s/mock_cc.sh", dir);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/args.log", dir);

    char script[PATH_MAX * 2];
    snprintf(script,
             sizeof(script),
             "#!/usr/bin/env bash\n"
             "echo \"$@\" > %s\n"
             "out=\"\"\n"
             "prev=\"\"\n"
             "for arg in \"$@\"; do\n"
             "  if [ \"$prev\" = \"-o\" ]; then out=\"$arg\"; fi\n"
             "  prev=\"$arg\"\n"
             "done\n"
             "if [ -n \"$out\" ]; then : > \"$out\"; fi\n"
             "exit 0\n",
             log_path);
    assert(write_file_contents(cc_path, script));

#ifndef _WIN32
    assert(chmod(cc_path, 0700) == 0);
#endif

    nova_setenv("NOVA_CC", cc_path);

    const char *source =
        "module demo.flags\n"
        "fun main(): Number = 3\n";

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

    char object_path[PATH_MAX];
    snprintf(object_path, sizeof(object_path), "%s/out.o", dir);
    char error[256] = {0};
    bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
    assert(ok);

    char *args = read_file_contents(log_path);
    assert(args != NULL);
    assert(strstr(args, "-O3") != NULL);
    assert(strstr(args, "-flto") != NULL);
    assert(strstr(args, "-fno-plt") != NULL);
    assert(strstr(args, "-fomit-frame-pointer") != NULL);
    assert(strstr(args, "-c") != NULL);

    free(args);
    nova_setenv("NOVA_CC", NULL);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);

    char cleanup_cmd[PATH_MAX * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", dir);
    system(cleanup_cmd);
}

static void test_aot_executable_generation(void) {
    const char *source =
        "module demo.aot\n"
        "fun app_entry(): Number = 7\n";

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

    const char *exe_path = "build/nova-aot-sample";
    char error[256] = {0};
    bool ok = nova_codegen_emit_executable(ir, &ctx, exe_path, "app_entry", error, sizeof(error));
    assert(ok && "AOT executable generation failed");

    struct stat st;
    assert(stat(exe_path, &st) == 0);

#ifndef _WIN32
    int rc = system("./build/nova-aot-sample");
    assert(WIFEXITED(rc));
    assert(WEXITSTATUS(rc) == 7);
#endif

    remove(exe_path);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
}

static void test_llvm_backend_codegen(void) {
    char template[] = "build/nova_llvmXXXXXX";
    char *dir = make_temp_dir(template);
    assert(dir != NULL);

    const char *source =
        "module demo.llvm\n"
        "fun app_entry(): Number = 9\n";

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

    nova_setenv("NOVA_CODEGEN_BACKEND", "llvm");
    char object_path[PATH_MAX];
    snprintf(object_path, sizeof(object_path), "%s/demo.o", dir);
    char error[256] = {0};
    bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
    assert(ok && "LLVM object emission failed");

    struct stat st;
    assert(stat(object_path, &st) == 0);

    char exe_path[PATH_MAX];
    snprintf(exe_path, sizeof(exe_path), "%s/demo-bin", dir);
    ok = nova_codegen_emit_executable(ir, &ctx, exe_path, "app_entry", error, sizeof(error));
    assert(ok && "LLVM executable emission failed");
    assert(stat(exe_path, &st) == 0);

#ifndef _WIN32
    char run_cmd[PATH_MAX * 2];
    snprintf(run_cmd, sizeof(run_cmd), "%s", exe_path);
    int rc = system(run_cmd);
    assert(WIFEXITED(rc));
    assert(WEXITSTATUS(rc) == 9);
#endif

    nova_setenv("NOVA_CODEGEN_BACKEND", NULL);
    remove(object_path);
    remove(exe_path);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);

    char cleanup_cmd[PATH_MAX * 2];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", dir);
    system(cleanup_cmd);
}


static void test_llvm_backend_stability_stress(void) {
    char *source = build_mock_stress_program(180, 20);
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

    nova_setenv("NOVA_CODEGEN_BACKEND", "llvm");
    for (size_t i = 0; i < 4; ++i) {
        char object_path[PATH_MAX];
        snprintf(object_path, sizeof(object_path), "build/llvm-stress-%zu.o", i);
        char error[256] = {0};
        bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
        assert(ok && "LLVM stress object emission failed");

        struct stat st;
        assert(stat(object_path, &st) == 0);
        remove(object_path);
    }

    char error[256] = {0};
    const char *exe_path = "build/llvm-stress-bin";
    bool ok = nova_codegen_emit_executable(ir, &ctx, exe_path, "run_179", error, sizeof(error));
    assert(ok && "LLVM stress executable emission failed");

#ifndef _WIN32
    int rc = system("./build/llvm-stress-bin");
    assert(WIFEXITED(rc));
    assert(WEXITSTATUS(rc) == 180);
#endif

    nova_setenv("NOVA_CODEGEN_BACKEND", NULL);
    remove(exe_path);

    nova_ir_free(ir);
    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
    free(source);
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
    test_gc_preserves_reachable_objects();
    test_gc_incremental_steps();
    test_gc_mock_allocator_and_failure();
    test_parser_and_semantics();
    test_lexer_keyword_classification();
    test_lexer_large_input_tokenization();
    test_match_exhaustiveness_warning();
    test_codegen_uses_low_latency_flags();
    test_aot_executable_generation();
    test_llvm_backend_codegen();
    test_llvm_backend_stability_stress();
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
