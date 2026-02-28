#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "generated_tokens.h"

typedef enum {
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
    NOVA_TOKEN_PIPE,
    NOVA_TOKEN_ARROW,
    NOVA_TOKEN_EFFECT,
    NOVA_TOKEN_TRUE,
    NOVA_TOKEN_FALSE,
    NOVA_TOKEN_NUMBER,
    NOVA_TOKEN_STRING,
    NOVA_TOKEN_IDENTIFIER,
    NOVA_TOKEN_LPAREN,
    NOVA_TOKEN_RPAREN,
    NOVA_TOKEN_LBRACE,
    NOVA_TOKEN_RBRACE,
    NOVA_TOKEN_LBRACKET,
    NOVA_TOKEN_RBRACKET,
    NOVA_TOKEN_COMMA,
    NOVA_TOKEN_DOT,
    NOVA_TOKEN_COLON,
    NOVA_TOKEN_SEMICOLON,
    NOVA_TOKEN_EQUAL,
    NOVA_TOKEN_ARROW_FN, /* alias for ARROW for clarity */
    NOVA_TOKEN_PIPE_OPERATOR, /* alias for PIPE */
    NOVA_TOKEN_BANG,
    NOVA_TOKEN_EOF,
    NOVA_TOKEN_ERROR
} NovaTokenType;

typedef struct {
    NovaTokenType type;
    const char *lexeme;
    size_t length;
    size_t line;
    size_t column;
} NovaToken;

typedef struct {
    NovaToken *data;
    size_t size;
    size_t capacity;
} NovaTokenArray;

void nova_token_array_init(NovaTokenArray *array);
bool nova_token_array_reserve(NovaTokenArray *array, size_t capacity);
void nova_token_array_push(NovaTokenArray *array, NovaToken token);
void nova_token_array_free(NovaTokenArray *array);

const char *nova_token_type_name(NovaTokenType type);
