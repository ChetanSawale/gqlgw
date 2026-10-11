/* errors.c */
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void errs_init(gql_errors *e, uint32_t max)
{
    vec_init(&e->items, sizeof(gql_err_item));
    e->max = max;
    e->truncated = 0;
}

void errs_free(gql_errors *e)
{
    uint32_t i;
    for (i = 0; i < e->items.len; i++)
        free(VEC_AT(&e->items, gql_err_item, i).msg);
    vec_free(&e->items);
}

int errs_count(const gql_errors *e)
{
    return (int)e->items.len;
}

int errs_full(const gql_errors *e)
{
    return e->max && e->items.len >= e->max;
}

void errs_addv(gql_errors *e, const gql_loc *locs, int nlocs, const char *fmt, va_list ap)
{
    gql_err_item *it;
    gql_buf b;
    int i;

    if (errs_full(e)) {
        e->truncated = 1;
        return;
    }
    buf_init(&b);
    buf_vprintf(&b, fmt, ap);
    it = vec_push(&e->items);
    if (!it) {
        buf_free(&b);
        return;
    }
    it->msg = b.p ? b.p : strdup("");
    if (b.p)
        b.p[b.len] = 0;
    if (nlocs > GQL_ERR_MAXLOCS)
        nlocs = GQL_ERR_MAXLOCS;
    for (i = 0; i < nlocs; i++)
        it->locs[i] = locs[i];
    it->nlocs = nlocs;
}

void errs_add(gql_errors *e, const gql_loc *locs, int nlocs, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    errs_addv(e, locs, nlocs, fmt, ap);
    va_end(ap);
}

void errs_to_json(const gql_errors *e, gql_buf *out)
{
    uint32_t i;
    int j;
    buf_putc(out, '[');
    for (i = 0; i < e->items.len; i++) {
        const gql_err_item *it = &VEC_AT(&e->items, gql_err_item, i);
        if (i)
            buf_putc(out, ',');
        buf_puts(out, "{\"message\":");
        buf_json_str(out, it->msg, strlen(it->msg));
        if (it->nlocs) {
            buf_puts(out, ",\"locations\":[");
            for (j = 0; j < it->nlocs; j++)
                buf_printf(out, "%s{\"line\":%u,\"column\":%u}", j ? "," : "",
                           it->locs[j].line, it->locs[j].col);
            buf_putc(out, ']');
        }
        buf_putc(out, '}');
    }
    buf_putc(out, ']');
}

void errs_to_text(const gql_errors *e, const char *file, gql_buf *out)
{
    uint32_t i;
    for (i = 0; i < e->items.len; i++) {
        const gql_err_item *it = &VEC_AT(&e->items, gql_err_item, i);
        if (it->nlocs)
            buf_printf(out, "%s:%u:%u: %s\n", file, it->locs[0].line, it->locs[0].col, it->msg);
        else
            buf_printf(out, "%s: %s\n", file, it->msg);
    }
    if (e->truncated)
        buf_printf(out, "%s: too many errors, output truncated\n", file);
}
