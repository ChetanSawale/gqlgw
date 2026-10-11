/* libFuzzer target: lexer + parser + value printer on arbitrary input. */
#include <stddef.h>
#include <stdint.h>

#include "parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    gql_arena a;
    gql_errors e;
    gql_parse_opts o = { 20000, 64 };
    ast_doc *d;
    arena_init(&a, 64u << 20);
    errs_init(&e, 4);
    d = gql_parse((const char *)data, size, &a, &o, &e);
    if (d) {
        /* print every default / argument value we can reach */
        const ast_def *def;
        gql_buf b;
        buf_init(&b);
        for (def = d->defs; def; def = def->next) {
            const ast_vardef *v;
            for (v = def->vars; v; v = v->next)
                if (v->defval)
                    gql_print_value(&b, v->defval);
        }
        buf_free(&b);
    }
    errs_free(&e);
    arena_free(&a);
    return 0;
}
