/* parser.c - recursive descent parser for GraphQL (September 2025). */
#include "parser.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEST_MAX 512

typedef struct {
    gql_lexer lx;
    gql_arena *a;
    gql_errors *errs;
    uint32_t depth;          /* value / type nesting */
    uint32_t sel_depth;      /* selection set nesting */
    uint32_t max_sel_depth;
    int failed;
} P;

#define TOK (p->lx.tok)
#define FAILED() (p->failed)
#define BAIL(r) do { if (p->failed) return r; } while (0)

static void *alloc(P *p, size_t n)
{
    void *r = arena_alloc(p->a, n);
    if (!r && !p->failed) {
        p->failed = 1;
        errs_add(p->errs, &TOK.loc, 1, "Syntax Error: Document too large (memory limit reached).");
    }
    return r;
}
#define NEW(T) ((T *)alloc(p, sizeof(T)))

static void fail_at(P *p, gql_loc loc, const char *fmt, ...) GQL_PRINTF(3, 4);
static void fail_at(P *p, gql_loc loc, const char *fmt, ...)
{
    va_list ap;
    gql_buf b;
    if (p->failed)
        return;
    p->failed = 1;
    buf_init(&b);
    buf_puts(&b, "Syntax Error: ");
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    errs_add(p->errs, &loc, 1, "%s", buf_cstr(&b));
    buf_free(&b);
}

static void advance(P *p)
{
    if (p->failed)
        return;
    if (lex_next(&p->lx)) {
        p->failed = 1;
        errs_add(p->errs, &p->lx.err_loc, 1, "%s", p->lx.err);
    }
}

static void unexpected(P *p)
{
    char d[256];
    lex_describe(&TOK, d, sizeof d);
    fail_at(p, TOK.loc, "Unexpected %s.", d);
}

static int peek(P *p, gql_tok_kind k)
{
    return !p->failed && TOK.kind == k;
}

static int peek_kw(P *p, const char *kw)
{
    return !p->failed && TOK.kind == T_NAME && gql_str_eqz(TOK.val, kw);
}

static int peek_desc(P *p)
{
    return peek(p, T_STRING) || peek(p, T_BLOCK_STRING);
}

static int skip(P *p, gql_tok_kind k)
{
    if (peek(p, k)) {
        advance(p);
        return 1;
    }
    return 0;
}

static void expect(P *p, gql_tok_kind k)
{
    char d[256];
    if (p->failed)
        return;
    if (TOK.kind == k) {
        advance(p);
        return;
    }
    lex_describe(&TOK, d, sizeof d);
    fail_at(p, TOK.loc, "Expected %s, found %s.", tok_kind_desc(k), d);
}

static void expect_kw(P *p, const char *kw)
{
    char d[256];
    if (p->failed)
        return;
    if (peek_kw(p, kw)) {
        advance(p);
        return;
    }
    lex_describe(&TOK, d, sizeof d);
    fail_at(p, TOK.loc, "Expected \"%s\", found %s.", kw, d);
}

static gql_str parse_name(P *p, gql_loc *loc)
{
    gql_str s = { "", 0 };
    char d[256];
    if (p->failed)
        return s;
    if (TOK.kind != T_NAME) {
        lex_describe(&TOK, d, sizeof d);
        fail_at(p, TOK.loc, "Expected Name, found %s.", d);
        return s;
    }
    s = TOK.val;
    if (loc)
        *loc = TOK.loc;
    advance(p);
    return s;
}

static gql_str parse_desc(P *p)
{
    gql_str s = { "", 0 };
    if (peek_desc(p)) {
        s = TOK.val;
        advance(p);
    }
    return s;
}

static int enter(P *p)
{
    if (++p->depth > NEST_MAX) {
        fail_at(p, TOK.loc, "Document exceeds maximum nesting depth of %d.", NEST_MAX);
        return 0;
    }
    return 1;
}

/* ---- values ----------------------------------------------------------- */

static ast_value *parse_value(P *p, int is_const);

static ast_value *parse_value_inner(P *p, int is_const)
{
    ast_value *v;
    gql_str name;

    if (p->failed)
        return NULL;
    v = NEW(ast_value);
    if (!v)
        return NULL;
    v->loc = TOK.loc;
    switch (TOK.kind) {
    case T_LBRACKET: {
        ast_value **tail = &v->items;
        v->kind = AV_LIST;
        advance(p);
        while (!p->failed && !peek(p, T_RBRACKET)) {
            if (peek(p, T_EOF)) {
                expect(p, T_RBRACKET);
                break;
            }
            *tail = parse_value(p, is_const);
            BAIL(NULL);
            tail = &(*tail)->next;
        }
        expect(p, T_RBRACKET);
        break;
    }
    case T_LBRACE: {
        ast_objfield **tail = &v->fields;
        v->kind = AV_OBJECT;
        advance(p);
        while (!p->failed && !peek(p, T_RBRACE)) {
            ast_objfield *f = NEW(ast_objfield);
            BAIL(NULL);
            f->loc = TOK.loc;
            f->name = parse_name(p, NULL);
            expect(p, T_COLON);
            f->value = parse_value(p, is_const);
            BAIL(NULL);
            *tail = f;
            tail = &f->next;
        }
        expect(p, T_RBRACE);
        break;
    }
    case T_INT:
        v->kind = AV_INT;
        v->s = TOK.val;
        advance(p);
        break;
    case T_FLOAT:
        v->kind = AV_FLOAT;
        v->s = TOK.val;
        advance(p);
        break;
    case T_STRING:
    case T_BLOCK_STRING:
        v->kind = AV_STRING;
        v->block = TOK.kind == T_BLOCK_STRING;
        v->s = TOK.val;
        advance(p);
        break;
    case T_NAME:
        name = TOK.val;
        if (gql_str_eqz(name, "true") || gql_str_eqz(name, "false")) {
            v->kind = AV_BOOLEAN;
            v->boolean = name.p[0] == 't';
        } else if (gql_str_eqz(name, "null")) {
            v->kind = AV_NULL;
        } else {
            v->kind = AV_ENUM;
            v->s = name;
        }
        advance(p);
        break;
    case T_DOLLAR:
        if (is_const) {
            gql_loc l = TOK.loc;
            advance(p);
            if (peek(p, T_NAME))
                fail_at(p, l, "Unexpected variable \"$%.*s\" in constant value.",
                        (int)TOK.val.n, TOK.val.p);
            else
                unexpected(p);
            return NULL;
        }
        advance(p);
        v->kind = AV_VARIABLE;
        v->s = parse_name(p, NULL);
        break;
    default:
        unexpected(p);
        return NULL;
    }
    return p->failed ? NULL : v;
}

static ast_value *parse_value(P *p, int is_const)
{
    ast_value *v;
    if (!enter(p))
        return NULL;
    v = parse_value_inner(p, is_const);
    p->depth--;
    return v;
}

/* ---- types ------------------------------------------------------------ */

static ast_type *parse_type(P *p)
{
    ast_type *t, *nn;
    if (!enter(p))
        return NULL;
    t = NEW(ast_type);
    if (!t)
        return NULL;
    t->loc = TOK.loc;
    if (skip(p, T_LBRACKET)) {
        t->kind = AT_LIST;
        t->of = parse_type(p);
        expect(p, T_RBRACKET);
    } else {
        t->kind = AT_NAMED;
        t->name = parse_name(p, NULL);
    }
    p->depth--;
    BAIL(NULL);
    if (peek(p, T_BANG)) {
        advance(p);
        nn = NEW(ast_type);
        if (!nn)
            return NULL;
        nn->kind = AT_NONNULL;
        nn->loc = t->loc;
        nn->of = t;
        return nn;
    }
    return t;
}

/* ---- arguments / directives ------------------------------------------ */

static ast_arg *parse_arguments(P *p, int is_const)
{
    ast_arg *head = NULL, **tail = &head;
    if (!peek(p, T_LPAREN))
        return NULL;
    advance(p);
    do {
        ast_arg *a = NEW(ast_arg);
        BAIL(NULL);
        a->name = parse_name(p, &a->loc);
        expect(p, T_COLON);
        a->value = parse_value(p, is_const);
        BAIL(NULL);
        *tail = a;
        tail = &a->next;
    } while (!p->failed && !peek(p, T_RPAREN));
    expect(p, T_RPAREN);
    return p->failed ? NULL : head;
}

static ast_directive *parse_directives(P *p, int is_const)
{
    ast_directive *head = NULL, **tail = &head;
    while (peek(p, T_AT)) {
        ast_directive *d = NEW(ast_directive);
        BAIL(NULL);
        d->loc = TOK.loc;
        advance(p);
        d->name = parse_name(p, NULL);
        d->args = parse_arguments(p, is_const);
        BAIL(NULL);
        *tail = d;
        tail = &d->next;
    }
    return head;
}

/* ---- selections ------------------------------------------------------- */

static ast_selset *parse_selset(P *p);

static ast_selection *parse_selection(P *p)
{
    ast_selection *s = NEW(ast_selection);
    if (!s)
        return NULL;
    s->loc = TOK.loc;
    if (peek(p, T_SPREAD)) {
        advance(p);
        if (peek(p, T_NAME) && !peek_kw(p, "on")) {
            s->kind = SEL_SPREAD;
            s->name = parse_name(p, &s->name_loc);
            s->dirs = parse_directives(p, 0);
            return p->failed ? NULL : s;
        }
        s->kind = SEL_INLINE;
        if (peek_kw(p, "on")) {
            advance(p);
            s->name = parse_name(p, &s->name_loc);
        }
        s->dirs = parse_directives(p, 0);
        s->selset = parse_selset(p);
        return p->failed ? NULL : s;
    }
    s->kind = SEL_FIELD;
    s->name = parse_name(p, &s->name_loc);
    if (skip(p, T_COLON)) {
        s->alias = s->name;
        s->name = parse_name(p, &s->name_loc);
    }
    s->args = parse_arguments(p, 0);
    s->dirs = parse_directives(p, 0);
    if (peek(p, T_LBRACE))
        s->selset = parse_selset(p);
    return p->failed ? NULL : s;
}

static ast_selset *parse_selset(P *p)
{
    ast_selset *ss;
    ast_selection **tail;
    BAIL(NULL);
    if (++p->sel_depth > p->max_sel_depth) {
        fail_at(p, TOK.loc, "Document exceeds maximum selection depth of %u.", p->max_sel_depth);
        return NULL;
    }
    ss = NEW(ast_selset);
    if (!ss)
        return NULL;
    ss->loc = TOK.loc;
    tail = &ss->first;
    expect(p, T_LBRACE);
    do {
        *tail = parse_selection(p);
        BAIL(NULL);
        tail = &(*tail)->next;
    } while (!p->failed && !peek(p, T_RBRACE));
    expect(p, T_RBRACE);
    p->sel_depth--;
    return p->failed ? NULL : ss;
}

/* ---- executable definitions ------------------------------------------- */

static ast_vardef *parse_vardefs(P *p)
{
    ast_vardef *head = NULL, **tail = &head;
    if (!peek(p, T_LPAREN))
        return NULL;
    advance(p);
    do {
        ast_vardef *v = NEW(ast_vardef);
        BAIL(NULL);
        v->desc = parse_desc(p);
        v->loc = TOK.loc;
        expect(p, T_DOLLAR);
        v->name = parse_name(p, &v->name_loc);
        expect(p, T_COLON);
        v->type = parse_type(p);
        if (skip(p, T_EQUALS))
            v->defval = parse_value(p, 1);
        v->dirs = parse_directives(p, 1);
        BAIL(NULL);
        *tail = v;
        tail = &v->next;
    } while (!p->failed && !peek(p, T_RPAREN));
    expect(p, T_RPAREN);
    return p->failed ? NULL : head;
}

static ast_def *parse_operation(P *p, gql_str desc, gql_loc loc)
{
    ast_def *d = NEW(ast_def);
    if (!d)
        return NULL;
    d->kind = DEF_OPERATION;
    d->desc = desc;
    d->loc = loc;
    if (peek(p, T_LBRACE)) {
        d->op = OP_QUERY;
        d->shorthand = 1;
        d->selset = parse_selset(p);
        return p->failed ? NULL : d;
    }
    if (peek_kw(p, "query"))
        d->op = OP_QUERY;
    else if (peek_kw(p, "mutation"))
        d->op = OP_MUTATION;
    else if (peek_kw(p, "subscription"))
        d->op = OP_SUBSCRIPTION;
    else {
        unexpected(p);
        return NULL;
    }
    advance(p);
    if (peek(p, T_NAME))
        d->name = parse_name(p, &d->name_loc);
    d->vars = parse_vardefs(p);
    d->dirs = parse_directives(p, 0);
    d->selset = parse_selset(p);
    return p->failed ? NULL : d;
}

static ast_def *parse_fragment(P *p, gql_str desc, gql_loc loc)
{
    ast_def *d = NEW(ast_def);
    if (!d)
        return NULL;
    d->kind = DEF_FRAGMENT;
    d->desc = desc;
    d->loc = loc;
    expect_kw(p, "fragment");
    if (peek_kw(p, "on")) {
        unexpected(p);
        return NULL;
    }
    d->name = parse_name(p, &d->name_loc);
    expect_kw(p, "on");
    d->type_cond = parse_name(p, &d->type_cond_loc);
    d->dirs = parse_directives(p, 0);
    d->selset = parse_selset(p);
    return p->failed ? NULL : d;
}

/* ---- type system definitions ----------------------------------------- */

static ast_inputval *parse_inputval(P *p)
{
    ast_inputval *v = NEW(ast_inputval);
    if (!v)
        return NULL;
    v->desc = parse_desc(p);
    v->name = parse_name(p, &v->loc);
    expect(p, T_COLON);
    v->type = parse_type(p);
    if (skip(p, T_EQUALS))
        v->defval = parse_value(p, 1);
    v->dirs = parse_directives(p, 1);
    return p->failed ? NULL : v;
}

static ast_inputval *parse_inputvals(P *p, gql_tok_kind open, gql_tok_kind close)
{
    ast_inputval *head = NULL, **tail = &head;
    if (!peek(p, open))
        return NULL;
    advance(p);
    do {
        *tail = parse_inputval(p);
        BAIL(NULL);
        tail = &(*tail)->next;
    } while (!p->failed && !peek(p, close));
    expect(p, close);
    return p->failed ? NULL : head;
}

static ast_fielddef *parse_fielddefs(P *p)
{
    ast_fielddef *head = NULL, **tail = &head;
    if (!peek(p, T_LBRACE))
        return NULL;
    advance(p);
    do {
        ast_fielddef *f = NEW(ast_fielddef);
        BAIL(NULL);
        f->desc = parse_desc(p);
        f->name = parse_name(p, &f->loc);
        f->args = parse_inputvals(p, T_LPAREN, T_RPAREN);
        expect(p, T_COLON);
        f->type = parse_type(p);
        f->dirs = parse_directives(p, 1);
        BAIL(NULL);
        *tail = f;
        tail = &f->next;
    } while (!p->failed && !peek(p, T_RBRACE));
    expect(p, T_RBRACE);
    return p->failed ? NULL : head;
}

static ast_name *new_name(P *p)
{
    ast_name *n = NEW(ast_name);
    if (!n)
        return NULL;
    n->name = parse_name(p, &n->loc);
    return p->failed ? NULL : n;
}

static ast_name *parse_implements(P *p)
{
    ast_name *head = NULL, **tail = &head;
    if (!peek_kw(p, "implements"))
        return NULL;
    advance(p);
    skip(p, T_AMP);
    do {
        *tail = new_name(p);
        BAIL(NULL);
        tail = &(*tail)->next;
    } while (skip(p, T_AMP));
    return p->failed ? NULL : head;
}

static ast_name *parse_union_members(P *p)
{
    ast_name *head = NULL, **tail = &head;
    if (!skip(p, T_EQUALS))
        return NULL;
    skip(p, T_PIPE);
    do {
        *tail = new_name(p);
        BAIL(NULL);
        tail = &(*tail)->next;
    } while (skip(p, T_PIPE));
    return p->failed ? NULL : head;
}

static ast_enumval *parse_enumvals(P *p)
{
    ast_enumval *head = NULL, **tail = &head;
    if (!peek(p, T_LBRACE))
        return NULL;
    advance(p);
    do {
        ast_enumval *v = NEW(ast_enumval);
        BAIL(NULL);
        v->desc = parse_desc(p);
        if (peek_kw(p, "true") || peek_kw(p, "false") || peek_kw(p, "null")) {
            fail_at(p, TOK.loc, "Name \"%.*s\" is reserved and cannot be used for an enum value.",
                    (int)TOK.val.n, TOK.val.p);
            return NULL;
        }
        v->name = parse_name(p, &v->loc);
        v->dirs = parse_directives(p, 1);
        BAIL(NULL);
        *tail = v;
        tail = &v->next;
    } while (!p->failed && !peek(p, T_RBRACE));
    expect(p, T_RBRACE);
    return p->failed ? NULL : head;
}

static const char *const DIRECTIVE_LOCATIONS[] = {
    "QUERY", "MUTATION", "SUBSCRIPTION", "FIELD", "FRAGMENT_DEFINITION", "FRAGMENT_SPREAD",
    "INLINE_FRAGMENT", "VARIABLE_DEFINITION", "SCHEMA", "SCALAR", "OBJECT", "FIELD_DEFINITION",
    "ARGUMENT_DEFINITION", "INTERFACE", "UNION", "ENUM", "ENUM_VALUE", "INPUT_OBJECT",
    "INPUT_FIELD_DEFINITION", "DIRECTIVE_DEFINITION", "FRAGMENT_VARIABLE_DEFINITION", NULL
};

static ast_name *parse_dir_locations(P *p)
{
    ast_name *head = NULL, **tail = &head;
    skip(p, T_PIPE);
    do {
        int i, ok = 0;
        if (peek(p, T_NAME))
            for (i = 0; DIRECTIVE_LOCATIONS[i]; i++)
                if (gql_str_eqz(TOK.val, DIRECTIVE_LOCATIONS[i]))
                    ok = 1;
        if (!ok) {
            unexpected(p);
            return NULL;
        }
        *tail = new_name(p);
        BAIL(NULL);
        tail = &(*tail)->next;
    } while (skip(p, T_PIPE));
    return p->failed ? NULL : head;
}

static ast_optype *parse_root_types(P *p)
{
    ast_optype *head = NULL, **tail = &head;
    expect(p, T_LBRACE);
    do {
        ast_optype *o = NEW(ast_optype);
        BAIL(NULL);
        o->loc = TOK.loc;
        if (peek_kw(p, "query"))
            o->op = OP_QUERY;
        else if (peek_kw(p, "mutation"))
            o->op = OP_MUTATION;
        else if (peek_kw(p, "subscription"))
            o->op = OP_SUBSCRIPTION;
        else {
            unexpected(p);
            return NULL;
        }
        advance(p);
        expect(p, T_COLON);
        o->type = parse_name(p, NULL);
        BAIL(NULL);
        *tail = o;
        tail = &o->next;
    } while (!p->failed && !peek(p, T_RBRACE));
    expect(p, T_RBRACE);
    return p->failed ? NULL : head;
}

static ast_def *parse_type_system(P *p, gql_str desc, gql_loc loc, int is_ext)
{
    ast_def *d = NEW(ast_def);
    if (!d)
        return NULL;
    d->desc = desc;
    d->loc = loc;
    d->is_ext = is_ext;

    if (peek_kw(p, "schema")) {
        d->kind = DEF_SCHEMA;
        advance(p);
        d->dirs = parse_directives(p, 1);
        if (!is_ext || peek(p, T_LBRACE))
            d->roots = parse_root_types(p);
        BAIL(NULL);
        if (is_ext && !d->dirs && !d->roots) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (peek_kw(p, "scalar")) {
        d->kind = DEF_SCALAR;
        advance(p);
        d->name = parse_name(p, &d->name_loc);
        d->dirs = parse_directives(p, 1);
        BAIL(NULL);
        if (is_ext && !d->dirs) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (peek_kw(p, "type") || peek_kw(p, "interface")) {
        d->kind = peek_kw(p, "type") ? DEF_OBJECT : DEF_INTERFACE;
        advance(p);
        d->name = parse_name(p, &d->name_loc);
        d->names = parse_implements(p);
        d->dirs = parse_directives(p, 1);
        d->fields = parse_fielddefs(p);
        BAIL(NULL);
        if (is_ext && !d->names && !d->dirs && !d->fields) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (peek_kw(p, "union")) {
        d->kind = DEF_UNION;
        advance(p);
        d->name = parse_name(p, &d->name_loc);
        d->dirs = parse_directives(p, 1);
        d->names = parse_union_members(p);
        BAIL(NULL);
        if (is_ext && !d->dirs && !d->names) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (peek_kw(p, "enum")) {
        d->kind = DEF_ENUM;
        advance(p);
        d->name = parse_name(p, &d->name_loc);
        d->dirs = parse_directives(p, 1);
        d->values = parse_enumvals(p);
        BAIL(NULL);
        if (is_ext && !d->dirs && !d->values) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (peek_kw(p, "input")) {
        d->kind = DEF_INPUT;
        advance(p);
        d->name = parse_name(p, &d->name_loc);
        d->dirs = parse_directives(p, 1);
        d->inputs = parse_inputvals(p, T_LBRACE, T_RBRACE);
        BAIL(NULL);
        if (is_ext && !d->dirs && !d->inputs) {
            unexpected(p);
            return NULL;
        }
        return d;
    }
    if (!is_ext && peek_kw(p, "directive")) {
        d->kind = DEF_DIRECTIVE;
        advance(p);
        expect(p, T_AT);
        d->name = parse_name(p, &d->name_loc);
        d->inputs = parse_inputvals(p, T_LPAREN, T_RPAREN);
        d->dirs = parse_directives(p, 1);
        if (peek_kw(p, "repeatable")) {
            d->repeatable = 1;
            advance(p);
        }
        expect_kw(p, "on");
        d->names = parse_dir_locations(p);
        return p->failed ? NULL : d;
    }
    unexpected(p);
    return NULL;
}

static ast_def *parse_definition(P *p)
{
    gql_str desc = { "", 0 };
    gql_loc loc = TOK.loc;
    int has_desc = 0;

    if (peek(p, T_LBRACE))
        return parse_operation(p, desc, loc);
    if (peek_desc(p)) {
        has_desc = 1;
        desc = parse_desc(p);
    }
    if (peek(p, T_NAME)) {
        if (peek_kw(p, "query") || peek_kw(p, "mutation") || peek_kw(p, "subscription"))
            return parse_operation(p, desc, loc);
        if (peek_kw(p, "fragment"))
            return parse_fragment(p, desc, loc);
        if (peek_kw(p, "extend")) {
            if (has_desc) {
                fail_at(p, loc, "Unexpected description, only GraphQL definitions support descriptions.");
                return NULL;
            }
            advance(p);
            return parse_type_system(p, desc, loc, 1);
        }
        if (peek_kw(p, "schema") || peek_kw(p, "scalar") || peek_kw(p, "type") ||
            peek_kw(p, "interface") || peek_kw(p, "union") || peek_kw(p, "enum") ||
            peek_kw(p, "input") || peek_kw(p, "directive"))
            return parse_type_system(p, desc, loc, 0);
    }
    if (has_desc && peek(p, T_LBRACE)) {
        fail_at(p, loc, "Unexpected description, descriptions are not supported on shorthand queries.");
        return NULL;
    }
    unexpected(p);
    return NULL;
}

ast_doc *gql_parse(const char *src, size_t n, gql_arena *a,
                   const gql_parse_opts *opts, gql_errors *errs)
{
    P pp, *p = &pp;
    ast_doc *doc;
    ast_def **tail;

    memset(p, 0, sizeof *p);
    p->a = a;
    p->errs = errs;
    p->max_sel_depth = (opts && opts->max_depth) ? opts->max_depth : NEST_MAX;
    lex_init(&p->lx, src, n, a, opts ? opts->max_tokens : 0);
    advance(p);
    doc = NEW(ast_doc);
    if (!doc)
        return NULL;
    tail = &doc->defs;
    do {
        *tail = parse_definition(p);
        if (p->failed)
            return NULL;
        tail = &(*tail)->next;
        doc->ndefs++;
    } while (!peek(p, T_EOF));
    doc->ntokens = p->lx.ntokens;
    return p->failed ? NULL : doc;
}

ast_value *gql_parse_const_value(const char *src, size_t n, gql_arena *a, gql_errors *errs)
{
    P pp, *p = &pp;
    ast_value *v;
    memset(p, 0, sizeof *p);
    p->a = a;
    p->errs = errs;
    p->max_sel_depth = NEST_MAX;
    lex_init(&p->lx, src, n, a, 0);
    advance(p);
    v = parse_value(p, 1);
    if (!p->failed && !peek(p, T_EOF))
        unexpected(p);
    return p->failed ? NULL : v;
}

/* ---- printing --------------------------------------------------------- */

/* graphql-js printString: escapes C0 / C1 control characters, quote and backslash */
static void print_string(gql_buf *b, const char *s, size_t n)
{
    size_t i;
    buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0x9F) {
            buf_printf(b, "\\u%04X", (unsigned char)s[i + 1]);
            i++;
            continue;
        }
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\f': buf_puts(b, "\\f"); break;
        case '\r': buf_puts(b, "\\r"); break;
        default:
            if (c < 0x20 || c == 0x7F)
                buf_printf(b, "\\u%04X", c);
            else
                buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t';
}

/* graphql-js printBlockString */
static void print_block_string(gql_buf *b, const char *s, size_t n)
{
    gql_buf esc;
    size_t i, nlines = 1, lines_after_first_ws = 1;
    int single, force_leading, has_trailing_triple, has_trailing_quote, has_trailing_slash, force_trailing,
        multi, skip_leading;
    const char *e;

    buf_init(&esc);
    for (i = 0; i < n; i++) {
        if (i + 2 < n && s[i] == '"' && s[i + 1] == '"' && s[i + 2] == '"') {
            buf_puts(&esc, "\\\"\"\"");
            i += 2;
        } else {
            buf_putc(&esc, s[i]);
        }
    }
    e = esc.p ? esc.p : "";
    /* lines after the first: every one empty or starting with whitespace */
    for (i = 0; i < esc.len; i++) {
        if (e[i] == '\r' || e[i] == '\n') {
            size_t j = i + ((e[i] == '\r' && i + 1 < esc.len && e[i + 1] == '\n') ? 2 : 1);
            nlines++;
            if (!(j >= esc.len || e[j] == '\n' || e[j] == '\r' || is_ws(e[j])))
                lines_after_first_ws = 0;
            i = j - 1;
        }
    }
    single = nlines == 1;
    force_leading = nlines > 1 && lines_after_first_ws;
    has_trailing_triple = esc.len >= 4 && memcmp(e + esc.len - 4, "\\\"\"\"", 4) == 0;
    has_trailing_quote = n && s[n - 1] == '"' && !has_trailing_triple;
    has_trailing_slash = n && s[n - 1] == '\\';
    force_trailing = has_trailing_quote || has_trailing_slash;
    multi = !single || n > 70 || force_trailing || force_leading || has_trailing_triple;
    skip_leading = single && n && is_ws(s[0]);
    buf_puts(b, "\"\"\"");
    if ((multi && !skip_leading) || force_leading)
        buf_putc(b, '\n');
    buf_put(b, e, esc.len);
    if (multi || force_trailing)
        buf_putc(b, '\n');
    buf_puts(b, "\"\"\"");
    buf_free(&esc);
}

static size_t js_len(const char *p, size_t n)
{
    size_t i, l = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c & 0xC0) != 0x80)
            l += c >= 0xF0 ? 2 : 1;
    }
    return l;
}

/* indent(join(items, "\n")) */
static void print_block_items(gql_buf *b, gql_buf *items, size_t nitems)
{
    size_t k, i;
    for (k = 0; k < nitems; k++) {
        if (k)
            buf_putc(b, '\n');
        buf_puts(b, "  ");
        for (i = 0; i < items[k].len; i++) {
            buf_putc(b, items[k].p[i]);
            if (items[k].p[i] == '\n')
                buf_puts(b, "  ");
        }
    }
}

void gql_print_value(gql_buf *b, const ast_value *v)
{
    const ast_value *it;
    const ast_objfield *f;
    gql_buf *items, line;
    size_t n = 0, k;

    switch (v->kind) {
    case AV_VARIABLE:
        buf_putc(b, '$');
        buf_put(b, v->s.p, v->s.n);
        return;
    case AV_INT: case AV_FLOAT: case AV_ENUM:
        buf_put(b, v->s.p, v->s.n);
        return;
    case AV_STRING:
        if (v->block)
            print_block_string(b, v->s.p, v->s.n);
        else
            print_string(b, v->s.p, v->s.n);
        return;
    case AV_BOOLEAN:
        buf_puts(b, v->boolean ? "true" : "false");
        return;
    case AV_NULL:
        buf_puts(b, "null");
        return;
    case AV_LIST:
        for (it = v->items; it; it = it->next)
            n++;
        break;
    case AV_OBJECT:
        for (f = v->fields; f; f = f->next)
            n++;
        break;
    }
    items = calloc(n ? n : 1, sizeof *items);
    if (!items) {
        b->oom = 1;
        return;
    }
    k = 0;
    if (v->kind == AV_LIST) {
        for (it = v->items; it; it = it->next) {
            buf_init(&items[k]);
            gql_print_value(&items[k++], it);
        }
    } else {
        for (f = v->fields; f; f = f->next) {
            buf_init(&items[k]);
            buf_put(&items[k], f->name.p, f->name.n);
            buf_puts(&items[k], ": ");
            gql_print_value(&items[k++], f->value);
        }
    }
    buf_init(&line);
    buf_puts(&line, v->kind == AV_LIST ? "[" : "{ ");
    for (k = 0; k < n; k++) {
        if (k)
            buf_puts(&line, ", ");
        buf_put(&line, items[k].p, items[k].len);
    }
    buf_puts(&line, v->kind == AV_LIST ? "]" : " }");
    if (js_len(line.p, line.len) > 80) {
        buf_puts(b, v->kind == AV_LIST ? "[\n" : "{\n");
        print_block_items(b, items, n);
        buf_puts(b, v->kind == AV_LIST ? "\n]" : "\n}");
    } else {
        buf_put(b, line.p, line.len);
    }
    buf_free(&line);
    for (k = 0; k < n; k++)
        buf_free(&items[k]);
    free(items);
}

void gql_print_type(gql_buf *b, const ast_type *t)
{
    switch (t->kind) {
    case AT_NAMED:
        buf_put(b, t->name.p, t->name.n);
        break;
    case AT_LIST:
        buf_putc(b, '[');
        gql_print_type(b, t->of);
        buf_putc(b, ']');
        break;
    case AT_NONNULL:
        gql_print_type(b, t->of);
        buf_putc(b, '!');
        break;
    }
}

const char *gql_op_name(gql_op_type op)
{
    switch (op) {
    case OP_QUERY: return "query";
    case OP_MUTATION: return "mutation";
    case OP_SUBSCRIPTION: return "subscription";
    }
    return "?";
}
