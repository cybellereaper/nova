#include "nova/token.h"

#include <stdlib.h>
#include <string.h>

void nova_token_array_init(NovaTokenArray *array) {
    array->data = NULL;
    array->size = 0;
    array->capacity = 0;
}

void nova_token_array_push(NovaTokenArray *array, NovaToken token) {
    if (array->size == array->capacity) {
        size_t new_capacity = array->capacity == 0 ? 16 : array->capacity * 2;
        NovaToken *new_data = (NovaToken *)realloc(array->data, new_capacity * sizeof(NovaToken));
        if (!new_data) {
            return;
        }
        array->data = new_data;
        array->capacity = new_capacity;
    }
    array->data[array->size++] = token;
}

void nova_token_array_free(NovaTokenArray *array) {
    free(array->data);
    array->data = NULL;
    array->size = array->capacity = 0;
}

const char *nova_token_type_name(NovaTokenType type) {
    switch (type) {
    case NOVA_TOKEN_MODULE: return "MODULE";
    case NOVA_TOKEN_IMPORT: return "IMPORT";
    case NOVA_TOKEN_FUN: return "FUN";
    case NOVA_TOKEN_LET: return "LET";
    case NOVA_TOKEN_TYPE: return "TYPE";
    case NOVA_TOKEN_IF: return "IF";
    case NOVA_TOKEN_WHILE: return "WHILE";
    case NOVA_TOKEN_ELSE: return "ELSE";
    case NOVA_TOKEN_MATCH: return "MATCH";
    case NOVA_TOKEN_ASYNC: return "ASYNC";
    case NOVA_TOKEN_AWAIT: return "AWAIT";
    case NOVA_TOKEN_PIPE: return "PIPE";
    case NOVA_TOKEN_ARROW: return "ARROW";
    case NOVA_TOKEN_EFFECT: return "EFFECT";
    case NOVA_TOKEN_TRUE: return "TRUE";
    case NOVA_TOKEN_FALSE: return "FALSE";
    case NOVA_TOKEN_NUMBER: return "NUMBER";
    case NOVA_TOKEN_STRING: return "STRING";
    case NOVA_TOKEN_IDENTIFIER: return "IDENTIFIER";
    case NOVA_TOKEN_LPAREN: return "LPAREN";
    case NOVA_TOKEN_RPAREN: return "RPAREN";
    case NOVA_TOKEN_LBRACE: return "LBRACE";
    case NOVA_TOKEN_RBRACE: return "RBRACE";
    case NOVA_TOKEN_LBRACKET: return "LBRACKET";
    case NOVA_TOKEN_RBRACKET: return "RBRACKET";
    case NOVA_TOKEN_COMMA: return "COMMA";
    case NOVA_TOKEN_DOT: return "DOT";
    case NOVA_TOKEN_COLON: return "COLON";
    case NOVA_TOKEN_SEMICOLON: return "SEMICOLON";
    case NOVA_TOKEN_EQUAL: return "EQUAL";
    case NOVA_TOKEN_ARROW_FN: return "ARROW_FN";
    case NOVA_TOKEN_PIPE_OPERATOR: return "PIPE_OPERATOR";
    case NOVA_TOKEN_BANG: return "BANG";
    case NOVA_TOKEN_EOF: return "EOF";
    case NOVA_TOKEN_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}
