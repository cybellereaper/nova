#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nova/parser.h"
#include "nova/semantic.h"

static const char *type_name(const NovaSemanticContext *ctx, NovaTypeId type) {
    const NovaTypeInfo *info = nova_semantic_type_info(ctx, type);
    if (!info) return "Unknown";
    switch (info->kind) {
    case NOVA_TYPE_KIND_NUMBER: return "Number";
    case NOVA_TYPE_KIND_STRING: return "String";
    case NOVA_TYPE_KIND_BOOL: return "Bool";
    case NOVA_TYPE_KIND_UNIT: return "Unit";
    case NOVA_TYPE_KIND_CUSTOM:
        if (info->as.custom.record) {
            return info->as.custom.record->decl->name.lexeme;
        }
        return "Custom";
    case NOVA_TYPE_KIND_FUNCTION: return "Function";
    case NOVA_TYPE_KIND_LIST: return "List";
    default: return "Unknown";
    }
}

int main(void) {
    char line[1024];
    printf("nova> ");
    while (fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, ":quit", 5) == 0) {
            break;
        }
        const char *header = "module repl.session\n";
        size_t source_len = strlen(header) + strlen("let it = ") + strlen(line) + 1;
        char *source = malloc(source_len);
        if (!source) {
            fprintf(stderr, "allocation failed\n");
            return 1;
        }
        snprintf(source, source_len, "%slet it = %s", header, line);

        NovaParser parser;
        nova_parser_init(&parser, source);
        NovaProgram *program = nova_parser_parse(&parser);
        if (!program || parser.had_error) {
            fprintf(stderr, "parse error (%zu issues)\n", parser.diagnostics.count);
            nova_parser_free(&parser);
            free(source);
            printf("nova> ");
            continue;
        }

        NovaSemanticContext ctx;
        nova_semantic_context_init(&ctx);
        nova_semantic_analyze_program(&ctx, program);
        if (ctx.diagnostics.count > 0) {
            fprintf(stderr, "semantic issues detected (%zu)\n", ctx.diagnostics.count);
        } else {
            const NovaLetDecl *decl = &program->decls[0].as.let_decl;
            const NovaExprInfo *info = nova_semantic_lookup_expr(&ctx, decl->value);
            if (info) {
                printf("=> %s\n", type_name(&ctx, info->type));
            } else {
                printf("=> Unknown\n");
            }
        }

        nova_semantic_context_free(&ctx);
        nova_program_free(program);
        free(program);
        nova_parser_free(&parser);
        free(source);
        printf("nova> ");
    }
    printf("bye\n");
    return 0;
}

