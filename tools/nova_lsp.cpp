#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nova/parser.h"
#include "nova/semantic.h"

static void send_response(const char *body) {
    size_t length = strlen(body);
    printf("Content-Length: %zu\r\n\r\n%s", length, body);
    fflush(stdout);
}

static char *read_file_contents(const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size < 0) {
        fclose(in);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(in);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, in);
    fclose(in);
    buffer[read] = '\0';
    return buffer;
}

static bool read_message(char **out_json) {
    char header[256];
    size_t content_length = 0;
    while (fgets(header, sizeof(header), stdin)) {
        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = (size_t)strtoul(header + 15, NULL, 10);
        }
        if (strcmp(header, "\r\n") == 0 || strcmp(header, "\n") == 0) {
            break;
        }
    }
    if (content_length == 0) {
        return false;
    }
    char *json = malloc(content_length + 1);
    if (!json) {
        return false;
    }
    size_t read_total = 0;
    while (read_total < content_length) {
        size_t chunk = fread(json + read_total, 1, content_length - read_total, stdin);
        if (chunk == 0) {
            free(json);
            return false;
        }
        read_total += chunk;
    }
    json[content_length] = '\0';
    *out_json = json;
    return true;
}

static bool json_extract_value(const char *json, const char *field, char *out, size_t out_size, bool *out_is_string) {
    const char *pos = strstr(json, field);
    if (!pos) return false;
    pos = strchr(pos, ':');
    if (!pos) return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos == '\"') {
        if (out_is_string) *out_is_string = true;
        pos++;
        size_t written = 0;
        while (*pos && *pos != '\"' && written + 1 < out_size) {
            if (*pos == '\\' && pos[1]) {
                pos++;
            }
            out[written++] = *pos++;
        }
        out[written] = '\0';
        return true;
    }
    if (out_is_string) *out_is_string = false;
    const char *end = pos;
    while (*end && *end != ',' && *end != '}' && *end != ']') end++;
    size_t len = (size_t)(end - pos);
    if (len + 1 > out_size) len = out_size - 1;
    memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static void decode_uri_component(const char *uri, char *out, size_t out_size) {
    size_t write_index = 0;
    for (size_t i = 0; uri[i] && write_index + 1 < out_size; ++i) {
        if (uri[i] == '%' && isxdigit((unsigned char)uri[i + 1]) && isxdigit((unsigned char)uri[i + 2])) {
            char hex[3] = { uri[i + 1], uri[i + 2], '\0' };
            out[write_index++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (uri[i] == '+') {
            out[write_index++] = ' ';
        } else {
            out[write_index++] = uri[i];
        }
    }
    out[write_index] = '\0';
}

static void uri_to_path(const char *uri, char *path, size_t path_size) {
    if (strncmp(uri, "file://", 7) == 0) {
        decode_uri_component(uri + 7, path, path_size);
    } else {
        decode_uri_component(uri, path, path_size);
    }
}

static const NovaToken *find_token_at(const NovaTokenArray *tokens, size_t line, size_t character) {
    for (size_t i = 0; i < tokens->size; ++i) {
        const NovaToken *token = &tokens->data[i];
        if (token->type == NOVA_TOKEN_EOF) break;
        size_t token_line = token->line > 0 ? token->line - 1 : 0;
        if (token_line > line) break;
        if (token_line < line) continue;
        size_t start_col = token->column > 0 ? token->column - 1 : 0;
        size_t end_col = start_col + token->length;
        if (character < start_col) break;
        if (character <= end_col) {
            return token;
        }
    }
    return NULL;
}

static const NovaExprInfo *find_expr_for_token(const NovaSemanticContext *ctx, const NovaToken *token) {
    const NovaExprInfo *best = NULL;
    for (size_t i = 0; i < ctx->expr_info.count; ++i) {
        const NovaExprInfo *info = &ctx->expr_info.items[i];
        const NovaExpr *expr = info->expr;
        if (!expr) continue;
        const NovaToken *start = &expr->start_token;
        if (start->line == token->line && start->column == token->column && start->lexeme == token->lexeme) {
            if (!best || expr->kind == NOVA_EXPR_IDENTIFIER) {
                best = info;
                if (expr->kind == NOVA_EXPR_IDENTIFIER) {
                    break;
                }
            }
        }
    }
    return best;
}

static void describe_type(const NovaSemanticContext *ctx, NovaTypeId type, char *buffer, size_t size) {
    const NovaTypeInfo *info = nova_semantic_type_info(ctx, type);
    if (!info) {
        snprintf(buffer, size, "Unknown");
        return;
    }
    switch (info->kind) {
    case NOVA_TYPE_KIND_NUMBER:
        snprintf(buffer, size, "Number");
        break;
    case NOVA_TYPE_KIND_STRING:
        snprintf(buffer, size, "String");
        break;
    case NOVA_TYPE_KIND_BOOL:
        snprintf(buffer, size, "Bool");
        break;
    case NOVA_TYPE_KIND_UNIT:
        snprintf(buffer, size, "Unit");
        break;
    case NOVA_TYPE_KIND_LIST:
        snprintf(buffer, size, "List");
        break;
    case NOVA_TYPE_KIND_FUNCTION:
        snprintf(buffer, size, "Function");
        break;
    case NOVA_TYPE_KIND_CUSTOM:
        if (info->as.custom.record && info->as.custom.record->decl) {
            size_t len = info->as.custom.record->decl->name.length;
            if (len + 1 > size) len = size - 1;
            memcpy(buffer, info->as.custom.record->decl->name.lexeme, len);
            buffer[len] = '\0';
        } else {
            snprintf(buffer, size, "Custom");
        }
        break;
    default:
        snprintf(buffer, size, "Unknown");
        break;
    }
}

static void send_null_response(const char *id, bool id_is_string) {
    char body[128];
    if (id_is_string) {
        snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":null}", id);
    } else {
        snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":null}", id);
    }
    send_response(body);
}

static void handle_initialize(const char *id, bool id_is_string) {
    char body[256];
    if (id_is_string) {
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{\"capabilities\":{\"hoverProvider\":true}}}",
                 id);
    } else {
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"capabilities\":{\"hoverProvider\":true}}}",
                 id);
    }
    send_response(body);
}

static void handle_hover(const char *id, bool id_is_string, const char *json) {
    char uri[512];
    if (!json_extract_value(json, "\"uri\"", uri, sizeof(uri), NULL)) {
        send_null_response(id, id_is_string);
        return;
    }
    char path[512];
    uri_to_path(uri, path, sizeof(path));
    char *source = read_file_contents(path);
    if (!source) {
        send_null_response(id, id_is_string);
        return;
    }
    NovaParser parser;
    nova_parser_init(&parser, source);
    NovaProgram *program = nova_parser_parse(&parser);
    if (!program || parser.had_error) {
        if (program) {
            nova_program_free(program);
            free(program);
        }
        nova_parser_free(&parser);
        free(source);
        send_null_response(id, id_is_string);
        return;
    }
    char line_buffer[32];
    char char_buffer[32];
    if (!json_extract_value(json, "\"line\"", line_buffer, sizeof(line_buffer), NULL) ||
        !json_extract_value(json, "\"character\"", char_buffer, sizeof(char_buffer), NULL)) {
        nova_program_free(program);
        free(program);
        nova_parser_free(&parser);
        free(source);
        send_null_response(id, id_is_string);
        return;
    }
    size_t line = (size_t)strtoul(line_buffer, NULL, 10);
    size_t character = (size_t)strtoul(char_buffer, NULL, 10);

    NovaSemanticContext ctx;
    nova_semantic_context_init(&ctx);
    nova_semantic_analyze_program(&ctx, program);

    const NovaToken *token = find_token_at(&parser.tokens, line, character);
    bool has_hover = false;
    char type_buffer[128];
    if (token) {
        const NovaExprInfo *info = find_expr_for_token(&ctx, token);
        if (info) {
            describe_type(&ctx, info->type, type_buffer, sizeof(type_buffer));
            has_hover = true;
        }
    }

    nova_semantic_context_free(&ctx);
    nova_program_free(program);
    free(program);
    nova_parser_free(&parser);
    free(source);

    if (!has_hover) {
        send_null_response(id, id_is_string);
        return;
    }

    char body[512];
    if (id_is_string) {
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":{\"contents\":{\"kind\":\"plaintext\",\"value\":\"Type: %s\"}}}",
                 id, type_buffer);
    } else {
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"contents\":{\"kind\":\"plaintext\",\"value\":\"Type: %s\"}}}",
                 id, type_buffer);
    }
    send_response(body);
}

int main(void) {
    bool shutdown_requested = false;
    char *json = NULL;
    while (read_message(&json)) {
        char method[64] = {0};
        json_extract_value(json, "\"method\"", method, sizeof(method), NULL);
        char id[64] = {0};
        bool id_is_string = false;
        bool has_id = json_extract_value(json, "\"id\"", id, sizeof(id), &id_is_string);

        if (strcmp(method, "initialize") == 0 && has_id) {
            handle_initialize(id, id_is_string);
        } else if (strcmp(method, "textDocument/hover") == 0 && has_id) {
            handle_hover(id, id_is_string, json);
        } else if (strcmp(method, "shutdown") == 0 && has_id) {
            send_null_response(id, id_is_string);
            shutdown_requested = true;
        } else if (strcmp(method, "exit") == 0) {
            if (shutdown_requested) {
                free(json);
                break;
            }
            free(json);
            return 1;
        } else if (has_id) {
            send_null_response(id, id_is_string);
        }

        free(json);
        json = NULL;
    }
    return 0;
}
