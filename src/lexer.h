/* lexer.h - GraphQL lexer (September 2025 spec). */
#ifndef GQL_LEXER_H
#define GQL_LEXER_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "util.h"

typedef enum {
    T_SOF, T_EOF,
    T_BANG, T_DOLLAR, T_AMP, T_LPAREN, T_RPAREN, T_SPREAD, T_COLON, T_EQUALS, T_AT,
    T_LBRACKET, T_RBRACKET, T_LBRACE, T_PIPE, T_RBRACE,
    T_NAME, T_INT, T_FLOAT, T_STRING, T_BLOCK_STRING
} gql_tok_kind;

typedef struct {
    gql_tok_kind kind;
    const char *start;   /* raw source position */
    uint32_t len;        /* raw length */
    gql_str val;         /* name / number text / decoded string value */
    gql_loc loc;
} gql_token;

typedef struct {
    const char *src, *end, *pos;
    uint32_t line;
    const char *line_start;
    const char *cpos;    /* column cache: position ... */
    uint32_t ccol;       /* ... and its 1-based column (UTF-16 units, like graphql-js) */
    gql_token tok;
    uint32_t ntokens, max_tokens;
    gql_arena *arena;
    int failed;
    char err[256];
    gql_loc err_loc;
} gql_lexer;

void lex_init(gql_lexer *lx, const char *src, size_t n, gql_arena *a, uint32_t max_tokens);
/* advance to next token; returns 0 on success, -1 on error (lx->err filled) */
int  lex_next(gql_lexer *lx);
/* human description of current token, e.g. <EOF>, "{", Name "foo" */
void lex_describe(const gql_token *t, char *out, size_t outn);
const char *tok_kind_desc(gql_tok_kind k);

#endif
