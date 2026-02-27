#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

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

static void print_diagnostics(const char *label, const NovaDiagnosticList *list) {
    if (!list || list->count == 0) {
        return;
    }
    fprintf(stderr, "%s diagnostics:\n", label);
    for (size_t i = 0; i < list->count; ++i) {
        const NovaDiagnostic *diag = &list->items[i];
        const char *severity = diag->severity == NOVA_DIAGNOSTIC_WARNING ? "warning" : "error";
        fprintf(stderr, "  %s at %zu:%zu: %s\n", severity, diag->token.line, diag->token.column, diag->message);
    }
}

static size_t diagnostic_count(const NovaDiagnosticList *list, NovaDiagnosticSeverity severity) {
    size_t count = 0;
    if (!list) return 0;
    for (size_t i = 0; i < list->count; ++i) {
        if (list->items[i].severity == severity) {
            count++;
        }
    }
    return count;
}

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [--strict] [--skip-codegen] <file>\n", argv0);
}

int main(int argc, char **argv) {
    bool strict = false;
    bool skip_codegen = false;
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--skip-codegen") == 0) {
            skip_codegen = true;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }

    char *source = read_file_contents(path);
    if (!source) {
        fprintf(stderr, "nova-check: failed to read %s\n", path);
        return 1;
    }

    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    if (!program || parser.had_error) {
        print_diagnostics("parser", &parser.diagnostics);
        nova_parser_free(&parser);
        free(source);
        return 1;
    }

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);
    print_diagnostics("semantic", &ctx.diagnostics);

    size_t warning_count = diagnostic_count(&ctx.diagnostics, NOVA_DIAGNOSTIC_WARNING);
    size_t error_count = diagnostic_count(&ctx.diagnostics, NOVA_DIAGNOSTIC_ERROR);
    if (error_count > 0 || (strict && warning_count > 0)) {
        nova_semantic_context_free(&ctx);
        nova_program_free(program);
        free(program);
        nova_parser_free(&parser);
        free(source);
        return 1;
    }

    if (!skip_codegen) {
        NovaIRProgram *ir = nova_ir_lower(program, &ctx);
        if (!ir) {
            fprintf(stderr, "nova-check: IR lowering failed\n");
            nova_semantic_context_free(&ctx);
            nova_program_free(program);
            free(program);
            nova_parser_free(&parser);
            free(source);
            return 1;
        }

        if (nova_mkdir("build", 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "nova-check: failed to create build directory\n");
            nova_ir_free(ir);
            nova_semantic_context_free(&ctx);
            nova_program_free(program);
            free(program);
            nova_parser_free(&parser);
            free(source);
            return 1;
        }

        char object_path[PATH_MAX];
        snprintf(object_path, sizeof(object_path), "build/nova-check-%ld.o", nova_process_id());
        char error[256] = {0};
        bool ok = nova_codegen_emit_object(ir, &ctx, object_path, error, sizeof(error));
        if (!ok) {
            fprintf(stderr, "nova-check: %s\n", error[0] ? error : "code generation failed");
            nova_ir_free(ir);
            nova_semantic_context_free(&ctx);
            nova_program_free(program);
            free(program);
            nova_parser_free(&parser);
            free(source);
            return 1;
        }
        remove(object_path);
        nova_ir_free(ir);
    }

    printf("nova-check: ok (%zu warnings)\n", warning_count);

    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
    free(source);
    return 0;
}
