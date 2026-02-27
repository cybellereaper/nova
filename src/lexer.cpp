#include "nova/lexer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool nova_match_keyword(const char *lexeme, size_t length, NovaTokenType *type) {
    struct Keyword { const char *text; NovaTokenType type; };
    static const struct Keyword keywords[] = {
        {"module", NOVA_TOKEN_MODULE},
        {"import", NOVA_TOKEN_IMPORT},
        {"fun", NOVA_TOKEN_FUN},
        {"let", NOVA_TOKEN_LET},
        {"type", NOVA_TOKEN_TYPE},
        {"if", NOVA_TOKEN_IF},
        {"while", NOVA_TOKEN_WHILE},
        {"else", NOVA_TOKEN_ELSE},
        {"match", NOVA_TOKEN_MATCH},
        {"async", NOVA_TOKEN_ASYNC},
        {"await", NOVA_TOKEN_AWAIT},
        {"true", NOVA_TOKEN_TRUE},
        {"false", NOVA_TOKEN_FALSE},
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strlen(keywords[i].text) == length && strncmp(keywords[i].text, lexeme, length) == 0) {
            *type = keywords[i].type;
            return true;
        }
    }
    return false;
}

void nova_lexer_init(NovaLexer *lexer, const char *source, size_t length) {
    lexer->source = source;
    lexer->length = length;
    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;
}

static char peek(const NovaLexer *lexer) {
    if (lexer->position >= lexer->length) {
        return '\0';
    }
    return lexer->source[lexer->position];
}

static char advance(NovaLexer *lexer) {
    char c = peek(lexer);
    if (c == '\0') return c;
    lexer->position++;
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static void skip_whitespace_and_comments(NovaLexer *lexer) {
    bool skipping = true;
    while (skipping) {
        skipping = false;
        char c = peek(lexer);
        switch (c) {
        case ' ': case '\r': case '\t': case '\n':
            advance(lexer);
            skipping = true;
            break;
        case '#':
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
            skipping = true;
            break;
        default:
            break;
        }
    }
}

static NovaToken make_token(NovaLexer *lexer, NovaTokenType type, size_t start, size_t length, size_t line, size_t column) {
    NovaToken token;
    token.type = type;
    token.lexeme = lexer->source + start;
    token.length = length;
    token.line = line;
    token.column = column;
    return token;
}

static NovaToken make_error(NovaLexer *lexer, size_t line, size_t column) {
    return make_token(lexer, NOVA_TOKEN_ERROR, lexer->position, 0, line, column);
}

static NovaToken lex_string(NovaLexer *lexer) {
    size_t start_pos = lexer->position;
    size_t line = lexer->line;
    size_t column = lexer->column;
    advance(lexer); // consume opening quote
    bool triple = false;
    if (peek(lexer) == '"' && lexer->position + 1 < lexer->length && lexer->source[lexer->position + 1] == '"') {
        triple = true;
        advance(lexer);
        advance(lexer);
    }
    while (true) {
        char c = peek(lexer);
        if (c == '\0') {
            return make_error(lexer, line, column);
        }
        if (!triple && c == '"') {
            advance(lexer);
            break;
        }
        if (triple && c == '"' && lexer->position + 2 < lexer->length && lexer->source[lexer->position + 1] == '"' && lexer->source[lexer->position + 2] == '"') {
            advance(lexer);
            advance(lexer);
            advance(lexer);
            break;
        }
        if (c == '\\') {
            advance(lexer);
            advance(lexer);
        } else {
            advance(lexer);
        }
    }
    size_t length = lexer->position - start_pos;
    return make_token(lexer, NOVA_TOKEN_STRING, start_pos, length, line, column);
}

static NovaToken lex_number(NovaLexer *lexer) {
    size_t start_pos = lexer->position;
    size_t line = lexer->line;
    size_t column = lexer->column;
    while (isdigit(peek(lexer))) {
        advance(lexer);
    }
    if (peek(lexer) == '.') {
        advance(lexer);
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
    }
    size_t length = lexer->position - start_pos;
    return make_token(lexer, NOVA_TOKEN_NUMBER, start_pos, length, line, column);
}

static NovaToken lex_identifier(NovaLexer *lexer) {
    size_t start_pos = lexer->position;
    size_t line = lexer->line;
    size_t column = lexer->column;
    while (isalnum(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }
    size_t length = lexer->position - start_pos;
    NovaTokenType type = NOVA_TOKEN_IDENTIFIER;
    if (nova_match_keyword(lexer->source + start_pos, length, &type)) {
        // keyword matched
    }
    return make_token(lexer, type, start_pos, length, line, column);
}

NovaToken nova_lexer_next(NovaLexer *lexer) {
    skip_whitespace_and_comments(lexer);
    size_t start = lexer->position;
    size_t line = lexer->line;
    size_t column = lexer->column;
    char c = peek(lexer);
    if (c == '\0') {
        return make_token(lexer, NOVA_TOKEN_EOF, start, 0, line, column);
    }
    if (isalpha(c) || c == '_') {
        return lex_identifier(lexer);
    }
    if (isdigit(c)) {
        return lex_number(lexer);
    }
    switch (c) {
    case '(': advance(lexer); return make_token(lexer, NOVA_TOKEN_LPAREN, start, 1, line, column);
    case ')': advance(lexer); return make_token(lexer, NOVA_TOKEN_RPAREN, start, 1, line, column);
    case '{': advance(lexer); return make_token(lexer, NOVA_TOKEN_LBRACE, start, 1, line, column);
    case '}': advance(lexer); return make_token(lexer, NOVA_TOKEN_RBRACE, start, 1, line, column);
    case '[': advance(lexer); return make_token(lexer, NOVA_TOKEN_LBRACKET, start, 1, line, column);
    case ']': advance(lexer); return make_token(lexer, NOVA_TOKEN_RBRACKET, start, 1, line, column);
    case ',': advance(lexer); return make_token(lexer, NOVA_TOKEN_COMMA, start, 1, line, column);
    case ';': advance(lexer); return make_token(lexer, NOVA_TOKEN_SEMICOLON, start, 1, line, column);
    case '.': advance(lexer); return make_token(lexer, NOVA_TOKEN_DOT, start, 1, line, column);
    case ':': advance(lexer); return make_token(lexer, NOVA_TOKEN_COLON, start, 1, line, column);
    case '=':
        advance(lexer);
        if (peek(lexer) == '>') {
            advance(lexer);
            return make_token(lexer, NOVA_TOKEN_ARROW_FN, start, 2, line, column);
        }
        return make_token(lexer, NOVA_TOKEN_EQUAL, start, 1, line, column);
    case '!':
        advance(lexer);
        return make_token(lexer, NOVA_TOKEN_BANG, start, 1, line, column);
    case '|':
        advance(lexer);
        if (peek(lexer) == '>') {
            advance(lexer);
            return make_token(lexer, NOVA_TOKEN_PIPE_OPERATOR, start, 2, line, column);
        }
        return make_token(lexer, NOVA_TOKEN_PIPE, start, 1, line, column);
    case '-':
        advance(lexer);
        if (peek(lexer) == '>') {
            advance(lexer);
            return make_token(lexer, NOVA_TOKEN_ARROW, start, 2, line, column);
        }
        break;
    case '"':
        return lex_string(lexer);
    default:
        break;
    }
    advance(lexer);
    return make_error(lexer, line, column);
}

NovaTokenArray nova_lexer_tokenize(const char *source) {
    NovaTokenArray array;
    nova_token_array_init(&array);
    NovaLexer lexer;
    size_t length = strlen(source);
    nova_lexer_init(&lexer, source, length);
    while (true) {
        NovaToken token = nova_lexer_next(&lexer);
        nova_token_array_push(&array, token);
        if (token.type == NOVA_TOKEN_EOF || token.type == NOVA_TOKEN_ERROR) {
            break;
        }
    }
    return array;
}
