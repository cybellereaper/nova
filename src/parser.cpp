#include "nova/parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NovaExpr *parse_expression(NovaParser *parser);
static NovaExpr *parse_block_expression(NovaParser *parser);

static NovaToken peek(const NovaParser *parser) {
    if (parser->current >= parser->tokens.size) {
        return (NovaToken){ .type = NOVA_TOKEN_EOF };
    }
    return parser->tokens.data[parser->current];
}

static NovaToken previous(const NovaParser *parser) {
    if (parser->current == 0) {
        return (NovaToken){ .type = NOVA_TOKEN_EOF };
    }
    return parser->tokens.data[parser->current - 1];
}

static bool is_at_end(const NovaParser *parser) {
    return peek(parser).type == NOVA_TOKEN_EOF;
}

static bool check(const NovaParser *parser, NovaTokenType type) {
    return peek(parser).type == type;
}

static NovaToken advance(NovaParser *parser) {
    if (!is_at_end(parser)) {
        parser->current++;
    }
    return previous(parser);
}

static void parser_error(NovaParser *parser, NovaToken token, const char *message) {
    if (parser->panic_mode) {
        return;
    }
    parser->panic_mode = true;
    parser->had_error = true;
    nova_diagnostic_list_push(&parser->diagnostics, (NovaDiagnostic){
        .token = token,
        .message = message,
        .severity = NOVA_DIAGNOSTIC_ERROR,
    });
}

static void synchronize(NovaParser *parser) {
    parser->panic_mode = false;
    while (!is_at_end(parser)) {
        if (previous(parser).type == NOVA_TOKEN_SEMICOLON) {
            return;
        }
        switch (peek(parser).type) {
        case NOVA_TOKEN_FUN:
        case NOVA_TOKEN_LET:
        case NOVA_TOKEN_TYPE:
        case NOVA_TOKEN_IF:
        case NOVA_TOKEN_WHILE:
        case NOVA_TOKEN_MATCH:
        case NOVA_TOKEN_ASYNC:
            return;
        default:
            break;
        }
        advance(parser);
    }
}

static bool match(NovaParser *parser, NovaTokenType type) {
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    return false;
}

static NovaToken consume(NovaParser *parser, NovaTokenType type, const char *message) {
    if (check(parser, type)) {
        return advance(parser);
    }
    parser_error(parser, peek(parser), message);
    return (NovaToken){ .type = NOVA_TOKEN_ERROR, .lexeme = message };
}

static NovaExpr *nova_expr_new(NovaExprKind kind, NovaToken start) {
    NovaExpr *expr = static_cast<NovaExpr *>(calloc(1, sizeof(NovaExpr)));
    if (!expr) {
        return NULL;
    }
    expr->kind = kind;
    expr->start_token = start;
    return expr;
}

static bool lookahead_lambda(const NovaParser *parser) {
    if (!check(parser, NOVA_TOKEN_LPAREN)) {
        return false;
    }
    size_t index = parser->current + 1;
    int paren_depth = 1;
    while (index < parser->tokens.size && paren_depth > 0) {
        NovaTokenType type = parser->tokens.data[index].type;
        if (type == NOVA_TOKEN_LPAREN) {
            return false;
        }
        if (type == NOVA_TOKEN_RPAREN) {
            paren_depth--;
            index++;
            break;
        }
        if (type == NOVA_TOKEN_IDENTIFIER || type == NOVA_TOKEN_COMMA || type == NOVA_TOKEN_COLON) {
            index++;
            continue;
        }
        return false;
    }
    if (paren_depth != 0 || index >= parser->tokens.size) {
        return false;
    }
    NovaTokenType next = parser->tokens.data[index].type;
    return next == NOVA_TOKEN_ARROW || next == NOVA_TOKEN_ARROW_FN;
}

static NovaParam parse_param(NovaParser *parser) {
    NovaToken name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected parameter name");
    NovaParam param = { .name = name, .has_type = false };
    if (match(parser, NOVA_TOKEN_COLON)) {
        param.has_type = true;
        param.type_name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected type name");
    }
    return param;
}

static NovaParamList parse_param_list(NovaParser *parser, NovaTokenType terminator) {
    NovaParamList params;
    nova_param_list_init(&params);
    if (!check(parser, terminator)) {
        while (true) {
            NovaParam param = parse_param(parser);
            nova_param_list_push(&params, param);
            if (!match(parser, NOVA_TOKEN_COMMA)) {
                break;
            }
        }
    }
    consume(parser, terminator, "expected ')' after parameters");
    return params;
}

static NovaArgList parse_argument_list(NovaParser *parser) {
    NovaArgList list;
    nova_arg_list_init(&list);
    if (!check(parser, NOVA_TOKEN_RPAREN)) {
        while (true) {
            NovaArg arg = { .has_label = false, .value = NULL };
            if (check(parser, NOVA_TOKEN_IDENTIFIER)) {
                NovaToken potential = peek(parser);
                NovaTokenType next_type = NOVA_TOKEN_EOF;
                if (parser->current + 1 < parser->tokens.size) {
                    next_type = parser->tokens.data[parser->current + 1].type;
                }
                if (next_type == NOVA_TOKEN_EQUAL) {
                    advance(parser); // consume identifier
                    consume(parser, NOVA_TOKEN_EQUAL, "expected '=' in named argument");
                    arg.has_label = true;
                    arg.label = potential;
                    arg.value = parse_expression(parser);
                } else {
                    arg.value = parse_expression(parser);
                }
            } else {
                arg.value = parse_expression(parser);
            }
            nova_arg_list_push(&list, arg);
            if (!match(parser, NOVA_TOKEN_COMMA)) {
                break;
            }
        }
    }
    consume(parser, NOVA_TOKEN_RPAREN, "expected ')' after arguments");
    return list;
}

static NovaExpr *parse_lambda(NovaParser *parser) {
    NovaToken start = consume(parser, NOVA_TOKEN_LPAREN, "expected '(' to start lambda");
    NovaParamList params = parse_param_list(parser, NOVA_TOKEN_RPAREN);
    consume(parser, NOVA_TOKEN_ARROW, "expected '->' after lambda parameters");
    NovaExpr *body = parse_expression(parser);
    NovaExpr *expr = nova_expr_new(NOVA_EXPR_LAMBDA, start);
    expr->as.lambda.params = params;
    expr->as.lambda.body = body;
    expr->as.lambda.body_is_block = body && body->kind == NOVA_EXPR_BLOCK;
    return expr;
}

static NovaLiteral parse_literal_value(NovaParser *parser, NovaToken token) {
    (void)parser;
    NovaLiteral literal;
    literal.token = token;
    literal.elements.items = NULL;
    literal.elements.count = 0;
    literal.elements.capacity = 0;
    switch (token.type) {
    case NOVA_TOKEN_NUMBER:
        literal.kind = NOVA_LITERAL_NUMBER;
        break;
    case NOVA_TOKEN_STRING:
        literal.kind = NOVA_LITERAL_STRING;
        break;
    case NOVA_TOKEN_TRUE:
    case NOVA_TOKEN_FALSE:
        literal.kind = NOVA_LITERAL_BOOL;
        break;
    default:
        literal.kind = NOVA_LITERAL_UNIT;
        break;
    }
    return literal;
}

static NovaExpr *parse_list_literal(NovaParser *parser) {
    NovaToken start = consume(parser, NOVA_TOKEN_LBRACKET, "expected '['");
    NovaExpr *expr = nova_expr_new(NOVA_EXPR_LIST_LITERAL, start);
    nova_expr_list_init(&expr->as.literal.elements);
    expr->as.literal.kind = NOVA_LITERAL_LIST;
    expr->as.literal.token = start;
    if (!check(parser, NOVA_TOKEN_RBRACKET)) {
        while (true) {
            NovaExpr *item = parse_expression(parser);
            nova_expr_list_push(&expr->as.literal.elements, item);
            if (!match(parser, NOVA_TOKEN_COMMA)) {
                break;
            }
        }
    }
    consume(parser, NOVA_TOKEN_RBRACKET, "expected ']' to close list literal");
    return expr;
}

static NovaExpr *parse_primary(NovaParser *parser) {
    if (check(parser, NOVA_TOKEN_LPAREN) && lookahead_lambda(parser)) {
        return parse_lambda(parser);
    }
    NovaToken token = advance(parser);
    switch (token.type) {
    case NOVA_TOKEN_NUMBER:
    case NOVA_TOKEN_STRING:
    case NOVA_TOKEN_TRUE:
    case NOVA_TOKEN_FALSE: {
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_LITERAL, token);
        expr->as.literal = parse_literal_value(parser, token);
        return expr;
    }
    case NOVA_TOKEN_IDENTIFIER: {
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_IDENTIFIER, token);
        expr->as.identifier.name = token;
        return expr;
    }
    case NOVA_TOKEN_LPAREN: {
        NovaExpr *expr = parse_expression(parser);
        consume(parser, NOVA_TOKEN_RPAREN, "expected ')' after expression");
        NovaExpr *group = nova_expr_new(NOVA_EXPR_PAREN, token);
        group->as.inner = expr;
        return group;
    }
    case NOVA_TOKEN_LBRACE:
        parser->current--; // step back to reuse block parsing
        return parse_block_expression(parser);
    case NOVA_TOKEN_LBRACKET:
        parser->current--; // reuse list literal parser which expects leading '['
        return parse_list_literal(parser);
    default:
        parser_error(parser, token, "unexpected token in expression");
        return nova_expr_new(NOVA_EXPR_LITERAL, token);
    }
}

static NovaExpr *parse_call_expr(NovaParser *parser) {
    NovaExpr *expr = parse_primary(parser);
    while (true) {
        if (match(parser, NOVA_TOKEN_LPAREN)) {
            NovaArgList args = parse_argument_list(parser);
            NovaExpr *call = nova_expr_new(NOVA_EXPR_CALL, expr ? expr->start_token : peek(parser));
            call->as.call.callee = expr;
            call->as.call.args = args;
            expr = call;
        } else {
            break;
        }
    }
    return expr;
}

static NovaExpr *parse_pipe_expr(NovaParser *parser) {
    NovaExpr *left = parse_call_expr(parser);
    if (!match(parser, NOVA_TOKEN_PIPE_OPERATOR)) {
        return left;
    }
    NovaExpr *expr = nova_expr_new(NOVA_EXPR_PIPE, left ? left->start_token : peek(parser));
    expr->as.pipe.target = left;
    nova_expr_list_init(&expr->as.pipe.stages);
    do {
        NovaExpr *stage = parse_call_expr(parser);
        nova_expr_list_push(&expr->as.pipe.stages, stage);
    } while (match(parser, NOVA_TOKEN_PIPE_OPERATOR));
    return expr;
}

static NovaExpr *parse_unary_or_pipe(NovaParser *parser) {
    if (match(parser, NOVA_TOKEN_AWAIT)) {
        NovaToken start = previous(parser);
        NovaExpr *value = parse_expression(parser);
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_AWAIT, start);
        expr->as.unary.value = value;
        return expr;
    }
    if (match(parser, NOVA_TOKEN_BANG)) {
        NovaToken start = previous(parser);
        NovaExpr *value = parse_expression(parser);
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_EFFECT, start);
        expr->as.unary.value = value;
        return expr;
    }
    return parse_pipe_expr(parser);
}

static NovaExpr *parse_match_expr(NovaParser *parser, NovaToken start) {
    NovaExpr *expr = nova_expr_new(NOVA_EXPR_MATCH, start);
    expr->as.match_expr.scrutinee = parse_expression(parser);
    consume(parser, NOVA_TOKEN_LBRACE, "expected '{' after match expression");
    nova_match_arm_list_init(&expr->as.match_expr.arms);
    while (!check(parser, NOVA_TOKEN_RBRACE) && !is_at_end(parser)) {
        NovaToken constructor = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected pattern constructor");
        NovaParamList bindings;
        nova_param_list_init(&bindings);
        if (match(parser, NOVA_TOKEN_LPAREN)) {
            bindings = parse_param_list(parser, NOVA_TOKEN_RPAREN);
        }
        consume(parser, NOVA_TOKEN_ARROW, "expected '->' after match arm");
        NovaExpr *body = parse_expression(parser);
        NovaMatchArm arm;
        arm.name = constructor;
        arm.bindings = bindings;
        arm.body = body;
        nova_match_arm_list_push(&expr->as.match_expr.arms, arm);
        if (!match(parser, NOVA_TOKEN_SEMICOLON)) {
            // implicit separator: continue when encountering next identifier or closing brace
        }
    }
    consume(parser, NOVA_TOKEN_RBRACE, "expected '}' to close match");
    return expr;
}

static NovaExpr *parse_expression(NovaParser *parser) {
    if (match(parser, NOVA_TOKEN_IF)) {
        NovaToken start = previous(parser);
        NovaExpr *condition = parse_expression(parser);
        NovaExpr *then_branch = parse_block_expression(parser);
        NovaExpr *else_branch = NULL;
        if (match(parser, NOVA_TOKEN_ELSE)) {
            if (check(parser, NOVA_TOKEN_IF)) {
                else_branch = parse_expression(parser);
            } else {
                else_branch = parse_block_expression(parser);
            }
        }
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_IF, start);
        expr->as.if_expr.condition = condition;
        expr->as.if_expr.then_branch = then_branch;
        expr->as.if_expr.else_branch = else_branch;
        return expr;
    }
    if (match(parser, NOVA_TOKEN_WHILE)) {
        NovaToken start = previous(parser);
        NovaExpr *condition = parse_expression(parser);
        NovaExpr *body = parse_block_expression(parser);
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_WHILE, start);
        expr->as.while_expr.condition = condition;
        expr->as.while_expr.body = body;
        return expr;
    }
    if (match(parser, NOVA_TOKEN_MATCH)) {
        return parse_match_expr(parser, previous(parser));
    }
    if (match(parser, NOVA_TOKEN_ASYNC)) {
        NovaToken start = previous(parser);
        NovaExpr *block = parse_block_expression(parser);
        NovaExpr *expr = nova_expr_new(NOVA_EXPR_ASYNC, start);
        expr->as.unary.value = block;
        return expr;
    }
    return parse_unary_or_pipe(parser);
}

static NovaExpr *parse_block_expression(NovaParser *parser) {
    NovaToken start = consume(parser, NOVA_TOKEN_LBRACE, "expected '{'");
    NovaExpr *expr = nova_expr_new(NOVA_EXPR_BLOCK, start);
    nova_expr_list_init(&expr->as.block.expressions);
    while (!check(parser, NOVA_TOKEN_RBRACE) && !is_at_end(parser)) {
        NovaExpr *item = parse_expression(parser);
        nova_expr_list_push(&expr->as.block.expressions, item);
        if (!match(parser, NOVA_TOKEN_SEMICOLON)) {
            if (check(parser, NOVA_TOKEN_RBRACE)) {
                break;
            }
        }
    }
    consume(parser, NOVA_TOKEN_RBRACE, "expected '}' to close block");
    return expr;
}

static NovaVariantDecl parse_variant_decl(NovaParser *parser) {
    NovaVariantDecl variant;
    variant.name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected variant name");
    nova_param_list_init(&variant.payload);
    if (match(parser, NOVA_TOKEN_LPAREN)) {
        variant.payload = parse_param_list(parser, NOVA_TOKEN_RPAREN);
    }
    return variant;
}

static NovaVariantList parse_variant_list(NovaParser *parser) {
    NovaVariantList list;
    nova_variant_list_init(&list);
    NovaVariantDecl variant = parse_variant_decl(parser);
    nova_variant_list_push(&list, variant);
    while (match(parser, NOVA_TOKEN_PIPE)) {
        NovaVariantDecl next = parse_variant_decl(parser);
        nova_variant_list_push(&list, next);
    }
    return list;
}

static NovaModulePath parse_module_path(NovaParser *parser) {
    NovaModulePath path;
    nova_module_path_init(&path);
    NovaToken segment = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected identifier in module path");
    nova_module_path_push(&path, segment);
    while (match(parser, NOVA_TOKEN_DOT)) {
        NovaToken next = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected identifier after '.'");
        nova_module_path_push(&path, next);
    }
    return path;
}

static NovaModuleDecl parse_module_decl(NovaParser *parser) {
    consume(parser, NOVA_TOKEN_MODULE, "expected 'module' keyword");
    NovaModuleDecl decl;
    decl.path = parse_module_path(parser);
    return decl;
}

static NovaImportDecl parse_import_decl(NovaParser *parser) {
    consume(parser, NOVA_TOKEN_IMPORT, "expected 'import'");
    NovaImportDecl decl;
    decl.path = parse_module_path(parser);
    decl.symbols = NULL;
    decl.symbol_count = 0;
    decl.symbol_capacity = 0;
    if (match(parser, NOVA_TOKEN_LBRACE)) {
        while (!check(parser, NOVA_TOKEN_RBRACE) && !is_at_end(parser)) {
            if (decl.symbol_count == decl.symbol_capacity) {
                size_t new_capacity = decl.symbol_capacity == 0 ? 4 : decl.symbol_capacity * 2;
                NovaToken *symbols = static_cast<NovaToken *>(realloc(decl.symbols, new_capacity * sizeof(NovaToken)));
                if (!symbols) {
                    break;
                }
                decl.symbols = symbols;
                decl.symbol_capacity = new_capacity;
            }
            decl.symbols[decl.symbol_count++] = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected imported symbol name");
            if (!match(parser, NOVA_TOKEN_COMMA)) {
                break;
            }
        }
        consume(parser, NOVA_TOKEN_RBRACE, "expected '}' after import symbols");
    }
    return decl;
}

static NovaLetDecl parse_let_decl(NovaParser *parser) {
    NovaToken let_token = consume(parser, NOVA_TOKEN_LET, "expected 'let'");
    NovaLetDecl decl;
    decl.name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected identifier after 'let'");
    decl.has_type = false;
    if (match(parser, NOVA_TOKEN_COLON)) {
        decl.has_type = true;
        decl.type_name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected type name");
    }
    consume(parser, NOVA_TOKEN_EQUAL, "expected '=' in let declaration");
    decl.value = parse_expression(parser);
    (void)let_token;
    return decl;
}

static NovaFunDecl parse_fun_decl(NovaParser *parser) {
    consume(parser, NOVA_TOKEN_FUN, "expected 'fun'");
    NovaFunDecl decl;
    decl.name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected function name");
    consume(parser, NOVA_TOKEN_LPAREN, "expected '(' after function name");
    decl.params = parse_param_list(parser, NOVA_TOKEN_RPAREN);
    decl.has_return_type = false;
    if (match(parser, NOVA_TOKEN_COLON)) {
        decl.has_return_type = true;
        decl.return_type = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected return type");
    }
    consume(parser, NOVA_TOKEN_EQUAL, "expected '=' before function body");
    decl.body = parse_expression(parser);
    return decl;
}

static NovaTypeDecl parse_type_decl(NovaParser *parser) {
    consume(parser, NOVA_TOKEN_TYPE, "expected 'type'");
    NovaTypeDecl decl;
    decl.name = consume(parser, NOVA_TOKEN_IDENTIFIER, "expected type name");
    decl.kind = NOVA_TYPE_DECL_SUM;
    nova_variant_list_init(&decl.variants);
    nova_param_list_init(&decl.tuple_fields);
    if (match(parser, NOVA_TOKEN_EQUAL)) {
        decl.kind = NOVA_TYPE_DECL_SUM;
        decl.variants = parse_variant_list(parser);
    } else if (match(parser, NOVA_TOKEN_LPAREN)) {
        decl.kind = NOVA_TYPE_DECL_TUPLE;
        decl.tuple_fields = parse_param_list(parser, NOVA_TOKEN_RPAREN);
    } else {
        parser_error(parser, peek(parser), "expected '=' or '(' after type name");
    }
    return decl;
}

static NovaDecl parse_decl(NovaParser *parser) {
    NovaToken token = peek(parser);

    if (token.type == NOVA_TOKEN_TYPE) {
        NovaDecl decl = {};
        decl.kind = NOVA_DECL_TYPE;
        decl.as.type_decl = parse_type_decl(parser);
        return decl;
    }

    if (token.type == NOVA_TOKEN_FUN) {
        NovaDecl decl = {};
        decl.kind = NOVA_DECL_FUN;
        decl.as.fun_decl = parse_fun_decl(parser);
        return decl;
    }

    if (token.type == NOVA_TOKEN_LET) {
        NovaDecl decl = {};
        decl.kind = NOVA_DECL_LET;
        decl.as.let_decl = parse_let_decl(parser);
        return decl;
    }

    parser_error(parser, token, "unexpected top-level declaration");
    synchronize(parser);
    NovaDecl fallback = {};
    fallback.kind = NOVA_DECL_LET;
    return fallback;
}

void nova_parser_init(NovaParser *parser, const char *source) {
    parser->source = source;
    parser->current = 0;
    parser->panic_mode = false;
    parser->had_error = false;
    nova_diagnostic_list_init(&parser->diagnostics);
    parser->tokens = nova_lexer_tokenize(source);
}

NovaProgram *nova_parser_parse(NovaParser *parser) {
    NovaProgram *program = static_cast<NovaProgram *>(calloc(1, sizeof(NovaProgram)));
    if (!program) {
        return NULL;
    }
    nova_program_init(program);
    program->module_decl = parse_module_decl(parser);
    while (match(parser, NOVA_TOKEN_IMPORT)) {
        parser->current--; // rewind to let parse_import_decl consume keyword
        NovaImportDecl import_decl = parse_import_decl(parser);
        nova_program_add_import(program, import_decl);
    }
    while (!is_at_end(parser)) {
        NovaDecl decl = parse_decl(parser);
        nova_program_add_decl(program, decl);
    }
    return program;
}

void nova_parser_free(NovaParser *parser) {
    nova_token_array_free(&parser->tokens);
    nova_diagnostic_list_free(&parser->diagnostics);
    parser->tokens.data = NULL;
    parser->tokens.size = 0;
    parser->tokens.capacity = 0;
}
