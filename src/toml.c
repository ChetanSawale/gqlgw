/* toml.c - TOML 1.1.0 parser.
 *
 * Table definition rules follow the spec text (and tomllib's reading of it):
 *   - a [header] defines its table once; implicit super-tables may be defined later;
 *   - dotted keys define the tables they go through, which can then only be
 *     extended by dotted keys of the same section (or [sub.tables] headers);
 *   - inline tables and arrays are frozen once closed.
 */
#include "toml.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    F_EXPLICIT = 1,         /* table defined by a [header] / [[header]] element */
    F_FROZEN = 2,           /* inline table or array: closed */
    F_AOT = 4,              /* array created by [[header]] */
};

#define MAX_KEY_PARTS 128
#define MAX_DEPTH 128

typedef struct {
    toml_doc *doc;
    const char *src, *p, *e;
    uint32_t line;
    const char *line_start;
    const char *col_pos;    /* column cache: position and its column */
    uint32_t col;
    uint32_t next_id;
    uint32_t sect;
    int depth;
    gql_buf tmp;
    jmp_buf jb;
} P;

typedef struct {
    gql_str s;
    gql_loc loc;
    const char *at;         /* position in the source (s may live in the arena) */
} keypart;

/* ---- errors / positions ------------------------------------------------ */

static gql_loc loc_at(P *ps, const char *at)
{
    gql_loc l;
    const char *q;
    uint32_t col;
    if (at < ps->line_start) {
        /* before the current line (multi-line constructs): rescan from the start */
        const char *ls = ps->src;
        l.line = 1;
        for (q = ps->src; q < at; q++)
            if (*q == '\n') {
                l.line++;
                ls = q + 1;
            }
        for (col = 1, q = ls; q < at; q++)
            if (((unsigned char)*q & 0xC0) != 0x80)
                col++;
        l.col = col;
        return l;
    }
    if (ps->col_pos && ps->col_pos >= ps->line_start && ps->col_pos <= at) {
        q = ps->col_pos;
        col = ps->col;
    } else {
        q = ps->line_start;
        col = 1;
    }
    for (; q < at; q++)
        if (((unsigned char)*q & 0xC0) != 0x80)
            col++;
    ps->col_pos = at;
    ps->col = col;
    l.line = ps->line;
    l.col = col;
    return l;
}

static void fail(P *ps, const char *at, const char *fmt, ...) GQL_PRINTF(3, 4) __attribute__((noreturn));

static void fail(P *ps, const char *at, const char *fmt, ...)
{
    va_list ap;
    if (at > ps->e)
        at = ps->e;
    ps->doc->err_loc = loc_at(ps, at);
    va_start(ap, fmt);
    vsnprintf(ps->doc->err, sizeof ps->doc->err, fmt, ap);
    va_end(ap);
    longjmp(ps->jb, 1);
}

static void *alloc(P *ps, size_t n)
{
    void *r = arena_alloc(&ps->doc->arena, n);
    if (!r)
        fail(ps, ps->p, "out of memory");
    return r;
}

/* printable form of a key for messages */
static const char *kname(P *ps, gql_str k)
{
    static const char dots[] = "...";
    char *out = alloc(ps, 72);
    uint32_t i, n = k.n > 64 ? 64 : k.n;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)k.p[i];
        out[i] = c < 0x20 || c == 0x7f ? '?' : (char)c;
    }
    if (k.n > 64)
        memcpy(out + n, dots, sizeof dots);
    else
        out[n] = 0;
    return out;
}

static void newline_at(P *ps, const char *next)
{
    ps->line++;
    ps->line_start = next;
    ps->col_pos = NULL;
}

/* ---- tree -------------------------------------------------------------- */

static toml_value *new_value(P *ps, toml_kind k, const char *at)
{
    toml_value *v = alloc(ps, sizeof *v);
    v->kind = k;
    v->loc = loc_at(ps, at);
    if (k == TOML_TABLE)
        v->id = ps->next_id++;
    return v;
}

static toml_value *lookup(P *ps, const toml_value *t, gql_str k)
{
    uint32_t i = hm_get(&ps->doc->keys, t->id, k.p, k.n);
    return i == GQL_NONE ? NULL : VEC_AT(&ps->doc->members, toml_value *, i);
}

static void append(P *ps, toml_value *parent, toml_value *child)
{
    (void)ps;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    parent->count++;
}

static void insert(P *ps, toml_value *t, const keypart *k, toml_value *child)
{
    toml_value **slot = vec_push(&ps->doc->members);
    if (!slot || hm_put(&ps->doc->keys, t->id, k->s.p, k->s.n, ps->doc->members.len - 1) < 0)
        fail(ps, ps->p, "out of memory");
    *slot = child;
    child->key = k->s;
    child->key_loc = k->loc;
    append(ps, t, child);
}

/* marks a closed inline table / array and everything inside it as frozen */
static void freeze(P *ps, toml_value *v)
{
    gql_vec stack;
    vec_init(&stack, sizeof(toml_value *));
    v->flags |= F_FROZEN;
    for (;;) {
        toml_value *c, **slot;
        for (c = v->first; c; c = c->next) {
            if ((c->kind == TOML_TABLE || c->kind == TOML_ARRAY) && !(c->flags & F_FROZEN)) {
                c->flags |= F_FROZEN;
                if (!(slot = vec_push(&stack))) {
                    vec_free(&stack);
                    fail(ps, ps->p, "out of memory");
                }
                *slot = c;
            }
        }
        if (!stack.len)
            break;
        v = VEC_AT(&stack, toml_value *, --stack.len);
    }
    vec_free(&stack);
}

/* ---- lexical helpers --------------------------------------------------- */

static int is_ws(int c) { return c == ' ' || c == '\t'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_bare(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '-' || c == '_';
}
static int hexval(int c)
{
    if (is_digit(c))
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void skip_ws(P *ps)
{
    while (ps->p < ps->e && is_ws(*ps->p))
        ps->p++;
}

static void check_utf8(P *ps, const char *at, size_t *adv)
{
    int n = utf8_valid_seq((const unsigned char *)at, (size_t)(ps->e - at));
    if (!n)
        fail(ps, at, "invalid UTF-8");
    *adv = (size_t)n;
}

static void skip_comment(P *ps)
{
    ps->p++;    /* '#' */
    while (ps->p < ps->e && *ps->p != '\n') {
        unsigned char c = (unsigned char)*ps->p;
        size_t n = 1;
        if (c == '\r' && ps->p + 1 < ps->e && ps->p[1] == '\n')
            return;
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            fail(ps, ps->p, "control character U+%04X is not allowed in a comment", c);
        if (c >= 0x80)
            check_utf8(ps, ps->p, &n);
        ps->p += n;
    }
}

/* consumes LF or CRLF; returns 0 if there is no newline at p */
static int eat_newline(P *ps)
{
    if (ps->p < ps->e && *ps->p == '\n') {
        ps->p++;
    } else if (ps->p + 1 < ps->e && ps->p[0] == '\r' && ps->p[1] == '\n') {
        ps->p += 2;
    } else {
        return 0;
    }
    newline_at(ps, ps->p);
    return 1;
}

/* whitespace, comments and newlines (inside arrays and inline tables) */
static void skip_wcn(P *ps)
{
    for (;;) {
        skip_ws(ps);
        if (ps->p < ps->e && *ps->p == '#')
            skip_comment(ps);
        if (!eat_newline(ps))
            return;
    }
}

/* ---- strings ----------------------------------------------------------- */

static gql_str take_tmp(P *ps)
{
    gql_str s;
    if (ps->tmp.oom)
        fail(ps, ps->p, "out of memory");
    s.n = (uint32_t)ps->tmp.len;
    s.p = ps->tmp.len ? arena_strndup(&ps->doc->arena, ps->tmp.p, ps->tmp.len) : "";
    if (!s.p)
        fail(ps, ps->p, "out of memory");
    buf_reset(&ps->tmp);
    return s;
}

static void escape(P *ps, const char *start)
{
    const char *p = ps->p;   /* after the backslash */
    uint32_t cp = 0;
    int i, nd;
    char out[4];
    if (p >= ps->e)
        fail(ps, start, "unterminated string");
    switch (*p) {
    case 'b': buf_putc(&ps->tmp, '\b'); ps->p++; return;
    case 't': buf_putc(&ps->tmp, '\t'); ps->p++; return;
    case 'n': buf_putc(&ps->tmp, '\n'); ps->p++; return;
    case 'f': buf_putc(&ps->tmp, '\f'); ps->p++; return;
    case 'r': buf_putc(&ps->tmp, '\r'); ps->p++; return;
    case 'e': buf_putc(&ps->tmp, 0x1b); ps->p++; return;
    case '"': buf_putc(&ps->tmp, '"'); ps->p++; return;
    case '\\': buf_putc(&ps->tmp, '\\'); ps->p++; return;
    case 'x': nd = 2; break;
    case 'u': nd = 4; break;
    case 'U': nd = 8; break;
    default:
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f)
            fail(ps, p - 1, "invalid escape sequence \\%c", *p);
        fail(ps, p - 1, "invalid escape sequence");
    }
    for (i = 1; i <= nd; i++) {
        int h = p + i < ps->e ? hexval((unsigned char)p[i]) : -1;
        if (h < 0)
            fail(ps, p - 1, "\\%c escape needs %d hexadecimal digits", *p, nd);
        cp = cp * 16 + (uint32_t)h;
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        fail(ps, p - 1, "escape \\%c%.*s is not a Unicode scalar value", *p, nd, p + 1);
    buf_put(&ps->tmp, out, (size_t)utf8_encode(cp, out));
    ps->p = p + 1 + nd;
}

/* basic ("...") and multi-line basic ("""...""") strings */
static gql_str basic_string(P *ps, int ml)
{
    const char *start = ps->p;
    ps->p += ml ? 3 : 1;
    if (ml)
        eat_newline(ps);
    for (;;) {
        unsigned char c;
        size_t n = 1;
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated string");
        c = (unsigned char)*ps->p;
        if (c == '"') {
            int q = 0;
            if (!ml) {
                ps->p++;
                break;
            }
            while (ps->p + q < ps->e && ps->p[q] == '"')
                q++;
            if (q >= 3) {
                if (q > 5)
                    fail(ps, ps->p, "too many quotes at the end of a multi-line string");
                buf_put(&ps->tmp, ps->p, (size_t)(q - 3));
                ps->p += q;
                break;
            }
            buf_put(&ps->tmp, ps->p, (size_t)q);
            ps->p += q;
            continue;
        }
        if (c == '\\') {
            ps->p++;
            if (ml && ps->p < ps->e && (is_ws(*ps->p) || *ps->p == '\n' || *ps->p == '\r')) {
                /* line ending backslash */
                const char *bs = ps->p - 1;
                skip_ws(ps);
                if (!eat_newline(ps))
                    fail(ps, bs, "invalid escape sequence: a line ending backslash must be followed by a newline");
                for (;;) {
                    skip_ws(ps);
                    if (!eat_newline(ps))
                        break;
                }
                continue;
            }
            escape(ps, start);
            continue;
        }
        if (c == '\n' || (c == '\r' && ps->p + 1 < ps->e && ps->p[1] == '\n')) {
            if (!ml)
                fail(ps, start, "unterminated string");
            buf_putc(&ps->tmp, '\n');
            eat_newline(ps);
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            fail(ps, ps->p, "control character U+%04X must be escaped", c);
        if (c >= 0x80)
            check_utf8(ps, ps->p, &n);
        buf_put(&ps->tmp, ps->p, n);
        ps->p += n;
    }
    return take_tmp(ps);
}

/* literal ('...') and multi-line literal ('''...''') strings */
static gql_str literal_string(P *ps, int ml)
{
    const char *start = ps->p;
    ps->p += ml ? 3 : 1;
    if (ml)
        eat_newline(ps);
    for (;;) {
        unsigned char c;
        size_t n = 1;
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated string");
        c = (unsigned char)*ps->p;
        if (c == '\'') {
            int q = 0;
            if (!ml) {
                ps->p++;
                break;
            }
            while (ps->p + q < ps->e && ps->p[q] == '\'')
                q++;
            if (q >= 3) {
                if (q > 5)
                    fail(ps, ps->p, "too many quotes at the end of a multi-line string");
                buf_put(&ps->tmp, ps->p, (size_t)(q - 3));
                ps->p += q;
                break;
            }
            buf_put(&ps->tmp, ps->p, (size_t)q);
            ps->p += q;
            continue;
        }
        if (c == '\n' || (c == '\r' && ps->p + 1 < ps->e && ps->p[1] == '\n')) {
            if (!ml)
                fail(ps, start, "unterminated string");
            buf_putc(&ps->tmp, '\n');
            eat_newline(ps);
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            fail(ps, ps->p, "control character U+%04X is not allowed in a literal string", c);
        if (c >= 0x80)
            check_utf8(ps, ps->p, &n);
        buf_put(&ps->tmp, ps->p, n);
        ps->p += n;
    }
    return take_tmp(ps);
}

static int starts3(const P *ps, char c)
{
    return ps->p + 2 < ps->e && ps->p[0] == c && ps->p[1] == c && ps->p[2] == c;
}

/* ---- keys -------------------------------------------------------------- */

static void simple_key(P *ps, keypart *k)
{
    const char *s = ps->p;
    k->loc = loc_at(ps, s);
    k->at = s;
    if (s >= ps->e)
        fail(ps, s, "expected a key");
    if (*s == '"' || *s == '\'') {
        if (starts3(ps, *s))
            fail(ps, s, "multi-line strings cannot be used as keys");
        k->s = *s == '"' ? basic_string(ps, 0) : literal_string(ps, 0);
        return;
    }
    while (ps->p < ps->e && is_bare((unsigned char)*ps->p))
        ps->p++;
    if (ps->p == s)
        fail(ps, s, "expected a key");
    k->s.p = s;
    k->s.n = (uint32_t)(ps->p - s);
}

static int parse_key(P *ps, keypart *parts)
{
    int n = 0;
    for (;;) {
        const char *save;
        if (n == MAX_KEY_PARTS)
            fail(ps, ps->p, "key has more than %d parts", MAX_KEY_PARTS);
        simple_key(ps, &parts[n++]);
        save = ps->p;
        skip_ws(ps);
        if (ps->p < ps->e && *ps->p == '.') {
            ps->p++;
            skip_ws(ps);
            continue;
        }
        ps->p = save;
        return n;
    }
}

/* ---- numbers and dates ------------------------------------------------- */

/* digits with single underscores between them; returns 0 if no digit at p */
static int scan_digits(P *ps, int base)
{
    const char *s = ps->p;
    for (;;) {
        int d = ps->p < ps->e ? hexval((unsigned char)*ps->p) : -1;
        if (d >= 0 && d < base) {
            ps->p++;
            continue;
        }
        if (ps->p < ps->e && *ps->p == '_') {
            int nd = ps->p + 1 < ps->e ? hexval((unsigned char)ps->p[1]) : -1;
            if (ps->p == s || nd < 0 || nd >= base)
                fail(ps, ps->p, "underscores in numbers must be surrounded by digits");
            ps->p++;
            continue;
        }
        return ps->p > s;
    }
}

static int is_value_end(P *ps)
{
    unsigned char c;
    if (ps->p >= ps->e)
        return 1;
    c = (unsigned char)*ps->p;
    return is_ws(c) || c == '\n' || c == '\r' || c == '#' || c == ',' || c == ']' || c == '}';
}

static void number(P *ps, toml_value *v)
{
    const char *s = ps->p, *q;
    char clean[512];
    size_t n = 0;
    int neg = 0, is_float = 0;

    if (*ps->p == '+' || *ps->p == '-') {
        neg = *ps->p == '-';
        ps->p++;
    }
    if (ps->e - ps->p >= 3 && (!memcmp(ps->p, "inf", 3) || !memcmp(ps->p, "nan", 3))) {
        v->kind = TOML_FLOAT;
        v->f = ps->p[0] == 'i' ? (neg ? -INFINITY : INFINITY) : (neg ? -NAN : NAN);
        ps->p += 3;
        return;
    }
    if (ps->p == s && ps->p + 1 < ps->e && ps->p[0] == '0' &&
        (ps->p[1] == 'x' || ps->p[1] == 'o' || ps->p[1] == 'b')) {
        int base = ps->p[1] == 'x' ? 16 : ps->p[1] == 'o' ? 8 : 2;
        uint64_t acc = 0;
        ps->p += 2;
        q = ps->p;
        if (ps->p < ps->e && *ps->p == '_')
            fail(ps, ps->p, "underscores in numbers must be surrounded by digits");
        if (!scan_digits(ps, base))
            fail(ps, q, "expected digits after the 0%c prefix", s[1]);
        for (; q < ps->p; q++) {
            if (*q == '_')
                continue;
            if (acc > (uint64_t)INT64_MAX / (uint64_t)base ||
                acc * (uint64_t)base + (uint64_t)hexval((unsigned char)*q) > (uint64_t)INT64_MAX)
                fail(ps, s, "integer is out of range (64-bit signed)");
            acc = acc * (uint64_t)base + (uint64_t)hexval((unsigned char)*q);
        }
        v->kind = TOML_INTEGER;
        v->i = (int64_t)acc;
        return;
    }
    q = ps->p;
    if (!scan_digits(ps, 10))
        fail(ps, s, "invalid value");
    if (*q == '0' && ps->p - q > 1)
        fail(ps, q, "leading zeros are not allowed");
    if (ps->p < ps->e && *ps->p == '.') {
        ps->p++;
        if (!scan_digits(ps, 10))
            fail(ps, ps->p, "expected a digit after the decimal point");
        is_float = 1;
    }
    if (ps->p < ps->e && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->e && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        if (!scan_digits(ps, 10))
            fail(ps, ps->p, "expected a digit in the exponent");
        is_float = 1;
    }
    for (q = s; q < ps->p; q++) {
        if (*q == '_')
            continue;
        if (n + 1 >= sizeof clean)
            fail(ps, s, "number is too long");
        clean[n++] = *q;
    }
    clean[n] = 0;
    if (is_float) {
        v->kind = TOML_FLOAT;
        v->f = strtod(clean, NULL);
    } else {
        const char *d = clean + (clean[0] == '+' || clean[0] == '-');
        uint64_t acc = 0, lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
        for (; *d; d++) {
            if (acc > (lim - (uint64_t)(*d - '0')) / 10)
                fail(ps, s, "integer is out of range (64-bit signed)");
            acc = acc * 10 + (uint64_t)(*d - '0');
        }
        v->kind = TOML_INTEGER;
        v->i = neg ? (int64_t)(0 - acc) : (int64_t)acc;
    }
}

static int two_digits(P *ps, const char *at, const char *what)
{
    if (at + 1 >= ps->e || !is_digit((unsigned char)at[0]) || !is_digit((unsigned char)at[1]))
        fail(ps, at, "expected two digits for the %s", what);
    return (at[0] - '0') * 10 + (at[1] - '0');
}

/* HH:MM[:SS[.frac]], appended to tmp as HH:MM:SS[.frac] */
static void time_part(P *ps)
{
    const char *s = ps->p;
    int h = two_digits(ps, s, "hour"), m, sec = 0;
    char out[48];
    if (s + 2 >= ps->e || s[2] != ':')
        fail(ps, s + 2, "expected ':' in time");
    m = two_digits(ps, s + 3, "minute");
    ps->p = s + 5;
    if (ps->p < ps->e && *ps->p == ':') {
        sec = two_digits(ps, ps->p + 1, "second");
        ps->p += 3;
    }
    if (h > 23 || m > 59 || sec > 59)
        fail(ps, s, "invalid time");
    snprintf(out, sizeof out, "%02d:%02d:%02d", h, m, sec);
    buf_puts(&ps->tmp, out);
    if (ps->p < ps->e && *ps->p == '.') {
        const char *f = ++ps->p;
        while (ps->p < ps->e && is_digit((unsigned char)*ps->p))
            ps->p++;
        if (ps->p == f)
            fail(ps, f, "expected digits after the decimal point in time");
        size_t nd = (size_t)(ps->p - f);
        buf_putc(&ps->tmp, '.');
        buf_put(&ps->tmp, f, nd > 9 ? 9 : nd);     /* nanoseconds: extra digits are truncated */
    }
}

static void datetime(P *ps, toml_value *v)
{
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const char *s = ps->p;
    char out[48];
    if (s + 2 < ps->e && s[2] == ':') {
        time_part(ps);
        v->kind = TOML_TIME_LOCAL;
        v->s = take_tmp(ps);
        return;
    }
    {
        int y = two_digits(ps, s, "year") * 100 + two_digits(ps, s + 2, "year");
        int mo, d, leap, dim;
        mo = (s + 7 < ps->e && s[7] == '-') ? two_digits(ps, s + 5, "month") : -1;
        if (mo < 0)
            fail(ps, s + 7, "expected '-' in date");
        d = two_digits(ps, s + 8, "day");
        leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        dim = mo >= 1 && mo <= 12 ? mdays[mo - 1] + (mo == 2 && leap) : 0;
        if (!dim || d < 1 || d > dim)
            fail(ps, s, "invalid date");
        snprintf(out, sizeof out, "%04d-%02d-%02d", y, mo, d);
        buf_puts(&ps->tmp, out);
        ps->p = s + 10;
    }
    if (ps->p < ps->e && (*ps->p == 'T' || *ps->p == 't' ||
                          (*ps->p == ' ' && ps->p + 3 < ps->e && is_digit((unsigned char)ps->p[1]) &&
                           is_digit((unsigned char)ps->p[2]) && ps->p[3] == ':'))) {
        ps->p++;
        buf_putc(&ps->tmp, 'T');
        time_part(ps);
        v->kind = TOML_DATETIME_LOCAL;
        if (ps->p < ps->e && (*ps->p == 'Z' || *ps->p == 'z')) {
            ps->p++;
            buf_putc(&ps->tmp, 'Z');
            v->kind = TOML_DATETIME;
        } else if (ps->p < ps->e && (*ps->p == '+' || *ps->p == '-')) {
            const char *o = ps->p;
            int oh = two_digits(ps, o + 1, "offset hour"), om;
            if (o + 3 >= ps->e || o[3] != ':')
                fail(ps, o + 3, "expected ':' in time offset");
            om = two_digits(ps, o + 4, "offset minute");
            if (oh > 23 || om > 59)
                fail(ps, o, "invalid time offset");
            snprintf(out, sizeof out, "%c%02d:%02d", *o, oh, om);
            buf_puts(&ps->tmp, out);
            ps->p = o + 6;
            v->kind = TOML_DATETIME;
        }
    } else {
        v->kind = TOML_DATE_LOCAL;
    }
    v->s = take_tmp(ps);
}

/* ---- values ------------------------------------------------------------ */

static toml_value *value(P *ps);
static void keyval(P *ps, toml_value *table, int in_inline);

static toml_value *array(P *ps)
{
    toml_value *v = new_value(ps, TOML_ARRAY, ps->p);
    const char *start = ps->p++;
    for (;;) {
        skip_wcn(ps);
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated array");
        if (*ps->p == ']') {
            ps->p++;
            break;
        }
        append(ps, v, value(ps));
        skip_wcn(ps);
        if (ps->p < ps->e && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->e && *ps->p == ']') {
            ps->p++;
            break;
        }
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated array");
        fail(ps, ps->p, "expected ',' or ']' in array");
    }
    freeze(ps, v);
    return v;
}

static toml_value *inline_table(P *ps)
{
    toml_value *v = new_value(ps, TOML_TABLE, ps->p);
    const char *start = ps->p++;
    for (;;) {
        skip_wcn(ps);
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated inline table");
        if (*ps->p == '}') {
            ps->p++;
            break;
        }
        keyval(ps, v, 1);
        skip_wcn(ps);
        if (ps->p < ps->e && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->e && *ps->p == '}') {
            ps->p++;
            break;
        }
        if (ps->p >= ps->e)
            fail(ps, start, "unterminated inline table");
        fail(ps, ps->p, "expected ',' or '}' in inline table");
    }
    freeze(ps, v);
    return v;
}

static toml_value *value(P *ps)
{
    const char *s = ps->p;
    toml_value *v;
    unsigned char c;
    if (s >= ps->e || *s == '\n' || *s == '\r' || *s == '#')
        fail(ps, s, "expected a value");
    c = (unsigned char)*s;
    if (c == '[' || c == '{') {
        if (++ps->depth > MAX_DEPTH)
            fail(ps, s, "values are nested more than %d levels deep", MAX_DEPTH);
        v = c == '[' ? array(ps) : inline_table(ps);
        ps->depth--;
        return v;
    }
    v = new_value(ps, TOML_STRING, s);
    if (c == '"') {
        v->s = basic_string(ps, starts3(ps, '"'));
    } else if (c == '\'') {
        v->s = literal_string(ps, starts3(ps, '\''));
    } else if (c == 't' && ps->e - s >= 4 && !memcmp(s, "true", 4)) {
        v->kind = TOML_BOOL;
        v->i = 1;
        ps->p += 4;
    } else if (c == 'f' && ps->e - s >= 5 && !memcmp(s, "false", 5)) {
        v->kind = TOML_BOOL;
        ps->p += 5;
    } else if (is_digit(c) && ((s + 4 < ps->e && is_digit((unsigned char)s[1]) && is_digit((unsigned char)s[2]) &&
                                 is_digit((unsigned char)s[3]) && s[4] == '-') ||
                                (s + 2 < ps->e && is_digit((unsigned char)s[1]) && s[2] == ':'))) {
        datetime(ps, v);
    } else if (is_digit(c) || c == '+' || c == '-' || c == 'i' || c == 'n') {
        number(ps, v);
    } else {
        fail(ps, s, "expected a value");
    }
    if (!is_value_end(ps))
        fail(ps, s, "invalid value");
    return v;
}

/* key = value, into a [table] section (in_inline = 0) or an inline table */
static void keyval(P *ps, toml_value *table, int in_inline)
{
    keypart parts[MAX_KEY_PARTS];
    int n = parse_key(ps, parts), i;
    toml_value *node = table, *v;
    skip_ws(ps);
    if (ps->p >= ps->e || *ps->p != '=')
        fail(ps, ps->p, "expected '=' after the key");
    ps->p++;
    skip_ws(ps);
    v = value(ps);
    for (i = 0; i < n - 1; i++) {
        toml_value *c = lookup(ps, node, parts[i].s);
        if (!c) {
            c = new_value(ps, TOML_TABLE, parts[i].at);
            c->loc = parts[i].loc;
            c->sect = in_inline ? 0 : ps->sect;
            insert(ps, node, &parts[i], c);
        } else if (c->kind != TOML_TABLE) {
            fail(ps, parts[i].at, "key \"%s\" is already defined as %s and cannot be used as a table",
                 kname(ps, parts[i].s), c->kind == TOML_ARRAY ? "an array" : "a value");
        } else if (c->flags & F_FROZEN) {
            fail(ps, parts[i].at, "cannot add keys to the inline table \"%s\"", kname(ps, parts[i].s));
        } else if (!in_inline) {
            if (c->flags & F_EXPLICIT)
                fail(ps, parts[i].at, "table \"%s\" is defined by a [header] and cannot be extended with dotted keys",
                     kname(ps, parts[i].s));
            if (c->sect && c->sect != ps->sect)
                fail(ps, parts[i].at, "table \"%s\" was defined by dotted keys in another section",
                     kname(ps, parts[i].s));
            c->sect = ps->sect;
        }
        node = c;
    }
    if (lookup(ps, node, parts[n - 1].s))
        fail(ps, parts[n - 1].at, "duplicate key \"%s\"", kname(ps, parts[n - 1].s));
    insert(ps, node, &parts[n - 1], v);
}

/* [table] and [[array.of.tables]] headers; returns the new current table */
static toml_value *header(P *ps)
{
    keypart parts[MAX_KEY_PARTS];
    const char *s = ps->p;
    int aot = ps->p + 1 < ps->e && ps->p[1] == '[';
    int n, i;
    toml_value *node, *c;
    ps->p += aot ? 2 : 1;
    skip_ws(ps);
    n = parse_key(ps, parts);
    skip_ws(ps);
    if (ps->p >= ps->e || *ps->p != ']' || (aot && (ps->p + 1 >= ps->e || ps->p[1] != ']')))
        fail(ps, ps->p, aot ? "expected ']]' to close the array of tables header" : "expected ']' to close the table header");
    ps->p += aot ? 2 : 1;

    node = ps->doc->root;
    for (i = 0; i < n - 1; i++) {
        c = lookup(ps, node, parts[i].s);
        if (!c) {
            c = new_value(ps, TOML_TABLE, parts[i].at);
            c->loc = parts[i].loc;
            insert(ps, node, &parts[i], c);
        } else if (c->kind == TOML_ARRAY && (c->flags & F_AOT)) {
            c = c->last;
        } else if (c->kind != TOML_TABLE) {
            fail(ps, parts[i].at, "key \"%s\" is already defined as %s and cannot be used as a table",
                 kname(ps, parts[i].s), c->kind == TOML_ARRAY ? "an array" : "a value");
        } else if (c->flags & F_FROZEN) {
            fail(ps, parts[i].at, "cannot add tables to the inline table \"%s\"", kname(ps, parts[i].s));
        }
        node = c;
    }
    c = lookup(ps, node, parts[n - 1].s);
    if (!aot) {
        if (!c) {
            c = new_value(ps, TOML_TABLE, s);
            insert(ps, node, &parts[n - 1], c);
        } else if (c->kind != TOML_TABLE) {
            fail(ps, parts[n - 1].at, c->kind == TOML_ARRAY && (c->flags & F_AOT)
                 ? "\"%s\" is an array of tables, not a table" : "key \"%s\" is already defined as a value",
                 kname(ps, parts[n - 1].s));
        } else if (c->flags & (F_EXPLICIT | F_FROZEN) || c->sect) {
            fail(ps, s, "table \"%s\" is already defined", kname(ps, parts[n - 1].s));
        } else {
            c->loc = loc_at(ps, s);
        }
        c->flags |= F_EXPLICIT;
        return c;
    }
    if (!c) {
        c = new_value(ps, TOML_ARRAY, s);
        c->flags |= F_AOT;
        insert(ps, node, &parts[n - 1], c);
    } else if (c->kind != TOML_ARRAY || !(c->flags & F_AOT)) {
        fail(ps, s, c->kind == TOML_TABLE ? "\"%s\" is already defined as a table, not an array of tables"
                                          : "cannot append to \"%s\": it is not an array of tables",
             kname(ps, parts[n - 1].s));
    }
    node = new_value(ps, TOML_TABLE, s);
    node->flags |= F_EXPLICIT;
    append(ps, c, node);
    return node;
}

/* ---- document ---------------------------------------------------------- */

int toml_parse(toml_doc *doc, const char *src, size_t n)
{
    P ps;
    toml_value *cur;
    memset(doc, 0, sizeof *doc);
    arena_init(&doc->arena, 0);
    hm_init(&doc->keys);
    vec_init(&doc->members, sizeof(toml_value *));
    memset(&ps, 0, sizeof ps);
    ps.doc = doc;
    ps.src = ps.p = src;
    ps.e = src + n;
    ps.line = 1;
    ps.line_start = src;
    ps.next_id = 1;
    ps.sect = 1;
    buf_init(&ps.tmp);
    if (setjmp(ps.jb)) {
        buf_free(&ps.tmp);
        doc->root = NULL;
        return -1;
    }
    doc->root = cur = new_value(&ps, TOML_TABLE, src);
    cur->flags |= F_EXPLICIT;
    if (n >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3))
        ps.p = ps.line_start = src + 3;
    for (;;) {
        skip_ws(&ps);
        if (ps.p >= ps.e)
            break;
        if (*ps.p == '[') {
            cur = header(&ps);
            ps.sect++;
        } else if (is_bare((unsigned char)*ps.p) || *ps.p == '"' || *ps.p == '\'') {
            keyval(&ps, cur, 0);
        } else if (*ps.p != '#' && *ps.p != '\n' && *ps.p != '\r') {
            fail(&ps, ps.p, "expected a key or a [table] header");
        }
        skip_ws(&ps);
        if (ps.p < ps.e && *ps.p == '#')
            skip_comment(&ps);
        if (ps.p >= ps.e)
            break;
        if (!eat_newline(&ps))
            fail(&ps, ps.p, *ps.p == '\r' ? "carriage return must be followed by a newline"
                                          : "expected a newline after the value");
    }
    buf_free(&ps.tmp);
    if (doc->keys.oom || doc->members.oom) {
        snprintf(doc->err, sizeof doc->err, "out of memory");
        return -1;
    }
    return 0;
}

void toml_free(toml_doc *doc)
{
    arena_free(&doc->arena);
    hm_free(&doc->keys);
    vec_free(&doc->members);
    doc->root = NULL;
}

const toml_value *toml_get(const toml_doc *doc, const toml_value *table, const char *key)
{
    uint32_t i;
    if (!table || table->kind != TOML_TABLE)
        return NULL;
    i = hm_get(&doc->keys, table->id, key, (uint32_t)strlen(key));
    return i == GQL_NONE ? NULL : VEC_AT(&doc->members, const toml_value *, i);
}

const char *toml_kind_name(toml_kind k)
{
    static const char *const names[] = {
        "string", "integer", "float", "bool", "datetime", "datetime-local", "date-local", "time-local",
        "array", "table"
    };
    return (unsigned)k < sizeof names / sizeof names[0] ? names[k] : "?";
}

void toml_to_tagged_json(const toml_value *v, gql_buf *b)
{
    const toml_value *c;
    char num[64];
    switch (v->kind) {
    case TOML_TABLE:
        buf_putc(b, '{');
        for (c = v->first; c; c = c->next) {
            buf_json_str(b, c->key.p, c->key.n);
            buf_putc(b, ':');
            toml_to_tagged_json(c, b);
            if (c->next)
                buf_putc(b, ',');
        }
        buf_putc(b, '}');
        return;
    case TOML_ARRAY:
        buf_putc(b, '[');
        for (c = v->first; c; c = c->next) {
            toml_to_tagged_json(c, b);
            if (c->next)
                buf_putc(b, ',');
        }
        buf_putc(b, ']');
        return;
    case TOML_INTEGER:
        snprintf(num, sizeof num, "%lld", (long long)v->i);
        break;
    case TOML_FLOAT:
        if (isnan(v->f))
            snprintf(num, sizeof num, "nan");
        else if (isinf(v->f))
            snprintf(num, sizeof num, v->f < 0 ? "-inf" : "inf");
        else
            snprintf(num, sizeof num, "%.17g", v->f);
        break;
    case TOML_BOOL:
        snprintf(num, sizeof num, v->i ? "true" : "false");
        break;
    default:
        break;
    }
    buf_puts(b, "{\"type\":\"");
    buf_puts(b, toml_kind_name(v->kind));
    buf_puts(b, "\",\"value\":");
    if (v->kind == TOML_INTEGER || v->kind == TOML_FLOAT || v->kind == TOML_BOOL)
        buf_json_str(b, num, strlen(num));
    else
        buf_json_str(b, v->s.p, v->s.n);
    buf_putc(b, '}');
}
