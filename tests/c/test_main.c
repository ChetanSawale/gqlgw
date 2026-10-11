/* C unit tests for libgql. */
#include <stdlib.h>

#include "gql.h"
#include "gqlb.h"
#include "json.h"
#include "parser.h"
#include "sdl.h"
#include "test.h"

int t_failures, t_checks;

/* ---- helpers --------------------------------------------------------------- */

static const char SCHEMA[] =
    "type Query {\n"
    "  user(id: ID!): User @fetch(source: \"users\", path: \"/users/{args.id}\")\n"
    "  users(first: Int = 10, filter: Filter): [User!]! @fetch(source: \"users\", path: \"/users\")\n"
    "  node(id: ID!): Node @fetch(source: \"nodes\", path: \"/nodes/{args.id}\")\n"
    "  search(q: String!): [Result!]! @fetch(source: \"s\", path: \"/s?q={args.q}\")\n"
    "  echo(e: Color = RED, l: [Int!], o: Pick): String @fetch(source: \"e\", path: \"/e\")\n"
    "}\n"
    "type Mutation { rename(id: ID!, name: String!): User @fetch(source: \"users\", method: \"PATCH\", path: \"/users/{args.id}\", body: \"{args}\") }\n"
    "interface Node { id: ID! }\n"
    "type User implements Node { id: ID! name: String friends: [User!]! }\n"
    "type Bot implements Node { id: ID! model: String }\n"
    "union Result = User | Bot\n"
    "input Filter { name: String, active: Boolean = true, tags: [String!] = [\"a\"] }\n"
    "input Pick @oneOf { a: Int, b: String }\n"
    "enum Color { RED GREEN }\n";

static gql_schema *schema_from(const char *sdl)
{
    gql_errors e, w;
    gql_compiled *c;
    gql_buf b;
    gql_schema *s = NULL;
    char err[256];
    errs_init(&e, 20);
    errs_init(&w, 20);
    c = gql_sdl_compile(sdl, strlen(sdl), &e, &w);
    if (!c) {
        gql_buf t;
        buf_init(&t);
        errs_to_text(&e, "sdl", &t);
        fprintf(stderr, "%s", buf_cstr(&t));
        buf_free(&t);
    } else {
        buf_init(&b);
        gqlb_serialize(gql_compiled_schema(c), &b);
        s = gqlb_load(b.p, b.len, err, sizeof err);
        if (!s)
            fprintf(stderr, "load: %s\n", err);
        buf_free(&b);
    }
    gql_compiled_free(c);
    errs_free(&e);
    errs_free(&w);
    return s;
}

static gql_limits LIM = { 0, 0, 0, 100, 0, 0, 1, 0 };

/* returns a malloc'd string: "ok" + plan dump of root args, or the errors JSON */
static char *run(gql_schema *s, const char *q, const char *vars, const char *op)
{
    gql_doc *d = gql_doc_new(s, q, strlen(q), &LIM);
    gql_buf out;
    char *r;
    buf_init(&out);
    if (!gql_doc_ok(d)) {
        size_t n;
        const char *e = gql_doc_errors(d, &n);
        buf_put(&out, e, n);
    } else {
        gql_plan *p = gql_plan_new(s, d, op ? op : "", op ? strlen(op) : 0, vars, vars ? strlen(vars) : 0, &LIM);
        if (!gql_plan_ok(p)) {
            size_t n;
            const char *e = gql_plan_errors(p, &n);
            buf_put(&out, e, n);
        } else {
            const gql_plan_view *v = gql_plan_get_view(p);
            const gql_plan_sel *root = &v->sels[v->root_sel];
            uint32_t k;
            buf_puts(&out, "ok");
            for (k = 0; k < root->fields_count; k++) {
                const gql_plan_field *f = &v->fields[root->fields_first + k];
                buf_printf(&out, " %.*s", (int)f->key_len, v->str + f->key_off);
                if (f->args_len)
                    buf_printf(&out, "=%.*s", (int)f->args_len, v->str + f->args_off);
            }
        }
        gql_plan_free(p);
    }
    gql_doc_free(d);
    r = strdup(buf_cstr(&out));
    buf_free(&out);
    return r;
}

#define RUNQ(s, q, vars, op, want) do { char *r_ = run(s, q, vars, op); CHECK_STR(r_, want); free(r_); } while (0)
#define RUNQ_HAS(s, q, vars, op, want) do { char *r_ = run(s, q, vars, op); CHECK_CONTAINS(r_, want); free(r_); } while (0)

/* ---- lexer / parser -------------------------------------------------------- */

static const ast_value *first_default(const char *q, gql_arena *a)
{
    gql_errors e;
    ast_doc *d;
    errs_init(&e, 4);
    d = gql_parse(q, strlen(q), a, NULL, &e);
    errs_free(&e);
    return d && d->defs->vars ? d->defs->vars->defval : NULL;
}

TEST(test_block_strings)
{
    gql_arena a;
    const ast_value *v;
    arena_init(&a, 0);
    v = first_default("query ($a: String = \"\"\"\n    hello\n      world\n    \"\"\") { x }", &a);
    CHECK(v && v->kind == AV_STRING && v->block);
    if (v)
        CHECK(v->s.n == 13 && memcmp(v->s.p, "hello\n  world", 13) == 0);
    v = first_default("query ($a: String = \"\"\"  \\\"\"\" \"\"\") { x }", &a);
    CHECK(v && v->s.n == 6 && memcmp(v->s.p, "  \"\"\" ", 6) == 0);
    arena_free(&a);
}

TEST(test_unicode_strings)
{
    gql_arena a;
    const ast_value *v;
    arena_init(&a, 0);
    v = first_default("query ($a: String = \"\\u00e9\\u{1F600}\\uD83D\\uDE00\") { x }", &a);
    CHECK(v && v->s.n == 10 && memcmp(v->s.p, "\xc3\xa9\xf0\x9f\x98\x80\xf0\x9f\x98\x80", 10) == 0);
    arena_free(&a);
}

TEST(test_syntax_errors)
{
    gql_arena a;
    gql_errors e;
    gql_buf b;
    struct { const char *q, *want; } cases[] = {
        { "{ a(x: 01) }", "1:9: Syntax Error: Invalid number, unexpected digit after 0: \"1\"." },
        { "{ a }}", "1:6: Syntax Error: Unexpected \"}\"." },
        { "\xc3\xa9 { a }", "1:1: Syntax Error: Unexpected character: \"\xc3\xa9\"." },
        { "{ \"\xf0\x9f\x98\x80\" x }", "1:3: Syntax Error: Expected Name, found String \"\xf0\x9f\x98\x80\"." },
        { "query { a(s: \"\xf0\x9f\x98\x80\") ; }", "1:20: Syntax Error: Unexpected character: \";\"." },
        { "{ a(s: \"\\x\") }", "1:9: Syntax Error: Invalid character escape sequence: \\\"x\"." },
        /* found by fuzzing: truncated \u escape at end of input (heap overflow) */
        { "\"ec \\u", "1:5: Syntax Error: Invalid Unicode escape sequence: \"\\u\"." },
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        arena_init(&a, 0);
        errs_init(&e, 4);
        buf_init(&b);
        CHECK(gql_parse(cases[i].q, strlen(cases[i].q), &a, NULL, &e) == NULL);
        errs_to_text(&e, "", &b);
        b.len--; /* trailing newline */
        CHECK_STR(buf_cstr(&b) + 1, cases[i].want);
        buf_free(&b);
        errs_free(&e);
        arena_free(&a);
    }
}

TEST(test_print_value)
{
    gql_arena a;
    gql_buf b;
    const ast_value *v;
    arena_init(&a, 0);
    buf_init(&b);
    v = first_default("query ($a: X = {b: [1, 2.5, \"s\\n\"], c: {}, d: null, e: RED}) { x }", &a);
    gql_print_value(&b, v);
    CHECK_STR(buf_cstr(&b), "{ b: [1, 2.5, \"s\\n\"], c: {  }, d: null, e: RED }");
    buf_free(&b);
    arena_free(&a);
}

/* ---- json ------------------------------------------------------------------- */

TEST(test_json)
{
    gql_arena a;
    char err[128];
    gql_json *j;
    gql_buf b;
    arena_init(&a, 0);
    buf_init(&b);
    {
        const char *js = "{\"a\": [1, -2.5e3, true, null], \"s\": \"x\\u00e9\\ud83d\\ude00\", \"o\": {}}";
        j = gql_json_parse(js, strlen(js), &a, 0, err, sizeof err);
    }
    CHECK(j != NULL);
    if (j) {
        gql_json_write(&b, j);
        CHECK_STR(buf_cstr(&b), "{\"a\":[1,-2.5e3,true,null],\"s\":\"x\xc3\xa9\xf0\x9f\x98\x80\",\"o\":{}}");
    }
    CHECK(gql_json_parse("{\"a\": 01}", 9, &a, 0, err, sizeof err) == NULL);
    CHECK(gql_json_parse("[1,]", 4, &a, 0, err, sizeof err) == NULL);
    CHECK(gql_json_parse("[[[[1]]]]", 9, &a, 3, err, sizeof err) == NULL);
    buf_free(&b);
    arena_free(&a);
}

/* ---- schema / plan --------------------------------------------------------- */

TEST(test_gqlb_roundtrip)
{
    gql_schema *s = schema_from(SCHEMA);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(gql_type_by_name(s, "User", 4) != GQL_NONE);
    CHECK(gql_type_by_name(s, "Nope", 4) == GQL_NONE);
    CHECK(gql_field_lookup(s, s->query_type, "__schema", 8) == s->f_schema);
    CHECK(gql_field_lookup(s, gql_type_by_name(s, "User", 4), "__schema", 8) == GQL_NONE);
    CHECK(gql_is_possible(s, gql_type_by_name(s, "Node", 4), gql_type_by_name(s, "Bot", 3)));
    gqlb_close(s);
}

TEST(test_arguments)
{
    gql_schema *s = schema_from(SCHEMA);
    if (!s)
        return;
    RUNQ(s, "{ users { id } }", NULL, NULL, "ok users={\"first\":10}");
    RUNQ(s, "{ users(filter: {name: \"x\"}) { id } }", NULL, NULL,
         "ok users={\"first\":10,\"filter\":{\"name\":\"x\",\"active\":true,\"tags\":[\"a\"]}}");
    RUNQ(s, "query($f: Filter) { users(first: 2, filter: $f) { id } }", "{\"f\":{\"tags\":\"one\"}}", NULL,
         "ok users={\"first\":2,\"filter\":{\"active\":true,\"tags\":[\"one\"]}}");
    RUNQ(s, "query($n: Int) { users(first: $n) { id } }", "{}", NULL, "ok users={\"first\":10}");
    RUNQ(s, "query($n: Int) { users(first: $n) { id } }", "{\"n\": null}", NULL, "ok users={\"first\":null}");
    RUNQ(s, "{ echo(l: 3, o: {b: \"x\"}) }", NULL, NULL, "ok echo={\"e\":\"RED\",\"l\":[3],\"o\":{\"b\":\"x\"}}");
    RUNQ(s, "query($id: ID!) { user(id: $id) { id } }", "{\"id\": 42}", NULL, "ok user={\"id\":\"42\"}");
    RUNQ(s, "{ a: user(id: 1) { id } b: user(id: \"2\") { id } }", NULL, NULL,
         "ok a={\"id\":\"1\"} b={\"id\":\"2\"}");
    gqlb_close(s);
}

TEST(test_variables_errors)
{
    gql_schema *s = schema_from(SCHEMA);
    if (!s)
        return;
    RUNQ_HAS(s, "query($id: ID!) { user(id: $id) { id } }", "{}", NULL,
             "Variable \\\"$id\\\" has invalid value: Expected a value of non-null type \\\"ID!\\\" to be provided.");
    RUNQ_HAS(s, "query($n: Int) { users(first: $n) { id } }", "{\"n\": 1.5}", NULL,
             "Variable \\\"$n\\\" has invalid value: Int cannot represent non-integer value: 1.5");
    RUNQ_HAS(s, "query($f: Filter) { users(filter: $f) { id } }", "{\"f\": {\"tags\": [1]}}", NULL,
             "Variable \\\"$f\\\" has invalid value at .tags[0]: String cannot represent a non string value: 1");
    RUNQ_HAS(s, "query($p: Pick) { echo(o: $p) }", "{\"p\": {\"a\": 1, \"b\": \"x\"}}", NULL,
             "exactly one field must be specified");
    RUNQ_HAS(s, "query($c: Color) { echo(e: $c) }", "{\"c\": \"BLUE\"}", NULL,
             "Value \\\"BLUE\\\" does not exist in \\\"Color\\\" enum.");
    RUNQ_HAS(s, "{ users { id } }", "[1]", NULL, "Variables must be provided as an object.");
    gqlb_close(s);
}

TEST(test_operations)
{
    gql_schema *s = schema_from(SCHEMA);
    if (!s)
        return;
    RUNQ_HAS(s, "query A { users { id } } query B { node(id: 1) { id } }", NULL, NULL,
             "Must provide operation name if query contains multiple operations.");
    RUNQ(s, "query A { users { id } } query B { node(id: 1) { id } }", NULL, "B", "ok node={\"id\":\"1\"}");
    RUNQ_HAS(s, "query A { users { id } }", NULL, "C", "Unknown operation named \\\"C\\\".");
    RUNQ(s, "mutation { rename(id: 1, name: \"x\") { id } }", NULL, NULL,
         "ok rename={\"id\":\"1\",\"name\":\"x\"}");
    RUNQ_HAS(s, "subscription { users { id } }", NULL, NULL, "The subscription operation is not supported");
    gqlb_close(s);
}

TEST(test_skip_include_and_merge)
{
    gql_schema *s = schema_from(SCHEMA);
    gql_doc *d;
    gql_plan *p;
    const char *q = "query($s: Boolean!) { users { id ...F @skip(if: $s) name } a: users { id } }\n"
                    "fragment F on User { name friends { id } }";
    if (!s)
        return;
    d = gql_doc_new(s, q, strlen(q), &LIM);
    CHECK(gql_doc_ok(d));
    p = gql_plan_new(s, d, "", 0, "{\"s\": false}", 12, &LIM);
    CHECK(gql_plan_ok(p));
    if (gql_plan_ok(p)) {
        const gql_plan_view *v = gql_plan_get_view(p);
        const gql_plan_sel *root = &v->sels[v->root_sel];
        const gql_plan_field *users = &v->fields[root->fields_first];
        const gql_plan_sel *us = &v->sels[users->child_first];
        CHECK(root->fields_count == 2);
        CHECK(us->fields_count == 3);   /* id, name, friends (name merged) */
    }
    gql_plan_free(p);
    p = gql_plan_new(s, d, "", 0, "{\"s\": true}", 11, &LIM);
    if (gql_plan_ok(p)) {
        const gql_plan_view *v = gql_plan_get_view(p);
        const gql_plan_field *users = &v->fields[v->sels[v->root_sel].fields_first];
        CHECK(v->sels[users->child_first].fields_count == 2);   /* id, name */
    }
    gql_plan_free(p);
    gql_doc_free(d);
    gqlb_close(s);
}

TEST(test_abstract_cases)
{
    gql_schema *s = schema_from(SCHEMA);
    gql_doc *d;
    gql_plan *p;
    const char *q = "{ search(q: \"x\") { __typename ... on User { name } ... on Node { id } } }";
    if (!s)
        return;
    d = gql_doc_new(s, q, strlen(q), &LIM);
    p = gql_plan_new(s, d, "", 0, NULL, 0, &LIM);
    CHECK(gql_plan_ok(p));
    if (gql_plan_ok(p)) {
        const gql_plan_view *v = gql_plan_get_view(p);
        const gql_plan_field *f = &v->fields[v->sels[v->root_sel].fields_first];
        uint32_t k;
        CHECK(f->kind == PF_ABSTRACT && f->child_count == 2);
        for (k = 0; k < f->child_count; k++) {
            const gql_plan_case *c = &v->cases[f->child_first + k];
            const char *tn = GQL_S(s, s->types[c->type].name);
            if (!strcmp(tn, "User"))
                CHECK(v->sels[c->sel].fields_count == 3);   /* __typename name id */
            else
                CHECK(v->sels[c->sel].fields_count == 2);   /* __typename id */
        }
    }
    gql_plan_free(p);
    gql_doc_free(d);
    gqlb_close(s);
}

TEST(test_plan_memo)
{
    gql_schema *s = schema_from(SCHEMA);
    gql_doc *d;
    gql_plan *p;
    /* 2^20 paths without memoization */
    const char *q = "{ users { ...A } }\n"
        "fragment A on User { friends { ...B } x1: friends { ...B } }\n"
        "fragment B on User { friends { ...C } x2: friends { ...C } }\n"
        "fragment C on User { friends { ...D } x3: friends { ...D } }\n"
        "fragment D on User { friends { ...E } x4: friends { ...E } }\n"
        "fragment E on User { id name }";
    if (!s)
        return;
    d = gql_doc_new(s, q, strlen(q), &LIM);
    CHECK(gql_doc_ok(d));
    p = gql_plan_new(s, d, "", 0, NULL, 0, &LIM);
    CHECK(gql_plan_ok(p));
    if (gql_plan_ok(p))
        CHECK(gql_plan_get_view(p)->nsels < 20);
    gql_plan_free(p);
    gql_doc_free(d);
    gqlb_close(s);
}

TEST(test_limits)
{
    gql_schema *s = schema_from(SCHEMA);
    gql_limits l = LIM;
    gql_doc *d;
    size_t n;
    if (!s)
        return;
    l.max_bytes = 10;
    d = gql_doc_new(s, "{ users { id } }", 16, &l);
    CHECK(!gql_doc_ok(d));
    CHECK_CONTAINS(gql_doc_errors(d, &n), "maximum size of 10 bytes");
    gql_doc_free(d);
    l = LIM;
    l.max_tokens = 5;
    d = gql_doc_new(s, "{ users { id } }", 16, &l);
    CHECK_CONTAINS(gql_doc_errors(d, &n), "more than 5 tokens");
    gql_doc_free(d);
    l = LIM;
    l.max_depth = 2;
    d = gql_doc_new(s, "{ users { friends { id } } }", 28, &l);
    CHECK_CONTAINS(gql_doc_errors(d, &n), "maximum selection depth of 2");
    gql_doc_free(d);
    l = LIM;
    l.introspection = 0;
    d = gql_doc_new(s, "{ __schema { queryType { name } } }", 35, &l);
    CHECK_CONTAINS(gql_doc_errors(d, &n), "introspection has been disabled");
    gql_doc_free(d);
    l = LIM;
    l.max_fields = 3;
    d = gql_doc_new(s, "{ users { id name friends { id } } }", 36, &l);
    {
        gql_plan *p = gql_plan_new(s, d, "", 0, NULL, 0, &l);
        CHECK(!gql_plan_ok(p));
        CHECK_CONTAINS(gql_plan_errors(p, &n), "exceeds 3 fields");
        gql_plan_free(p);
    }
    gql_doc_free(d);
    gqlb_close(s);
}

TEST(test_corrupted_gqlb)
{
    gql_errors e, w;
    gql_compiled *c;
    gql_buf b;
    char err[256];
    size_t i;
    int loaded = 0;
    errs_init(&e, 4);
    errs_init(&w, 4);
    c = gql_sdl_compile(SCHEMA, strlen(SCHEMA), &e, &w);
    buf_init(&b);
    gqlb_serialize(gql_compiled_schema(c), &b);
    /* flip bytes all over the file: loading must never crash */
    for (i = sizeof(gqlb_header); i < b.len; i += 7) {
        gql_schema *s;
        char saved = b.p[i];
        b.p[i] = (char)(saved ^ 0x5a);
        s = gqlb_load(b.p, b.len, err, sizeof err);
        if (s) {
            loaded++;
            gqlb_close(s);
        }
        b.p[i] = saved;
    }
    CHECK(gqlb_load(b.p, 10, err, sizeof err) == NULL);
    CHECK(loaded > 0);
    buf_free(&b);
    gql_compiled_free(c);
    errs_free(&e);
    errs_free(&w);
}

int main(void)
{
    RUN(test_block_strings);
    RUN(test_unicode_strings);
    RUN(test_syntax_errors);
    RUN(test_print_value);
    RUN(test_json);
    RUN(test_gqlb_roundtrip);
    RUN(test_arguments);
    RUN(test_variables_errors);
    RUN(test_operations);
    RUN(test_skip_include_and_merge);
    RUN(test_abstract_cases);
    RUN(test_plan_memo);
    RUN(test_limits);
    RUN(test_corrupted_gqlb);
    fprintf(stderr, "%d checks, %d failures\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
