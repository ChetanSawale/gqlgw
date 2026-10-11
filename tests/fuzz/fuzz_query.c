/* libFuzzer target: full pipeline (parse, validate, plan, introspection) against
 * the harness schema. Input: "<query>\0<variables json>\0<operation name>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gql.h"
#include "gqlb.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gql_schema *S;

static void load(void)
{
    char err[256];
    const char *path = getenv("GQL_FUZZ_SCHEMA");
    S = gqlb_open(path ? path : "build/harness.gqlb", err, sizeof err);
    if (!S) {
        fprintf(stderr, "fuzz_query: %s (set GQL_FUZZ_SCHEMA)\n", err);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *q = (const char *)data, *vars = "", *op = "";
    size_t qn = size, vn = 0, on = 0;
    const char *z;
    gql_limits lim = { 1 << 20, 20000, 64, 50, 20000, 64u << 20, 1, 0 };
    gql_doc *d;
    gql_plan *p;

    if (!S)
        load();
    if ((z = memchr(data, 0, size))) {
        const char *z2;
        qn = (size_t)(z - q);
        vars = z + 1;
        vn = size - qn - 1;
        if ((z2 = memchr(vars, 0, vn))) {
            op = z2 + 1;
            on = vn - (size_t)(z2 - vars) - 1;
            vn = (size_t)(z2 - vars);
        }
    }
    d = gql_doc_new(S, q, qn, &lim);
    if (d && gql_doc_ok(d)) {
        p = gql_plan_new(S, d, op, on, vars, vn, &lim);
        gql_plan_free(p);
    }
    gql_doc_free(d);
    return 0;
}
