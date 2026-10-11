/* json.c - RFC 8259 JSON parser into an arena + writer. */
#include "json.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *p, *e, *start;
    gql_arena *a;
    uint32_t depth, max_depth;
    char *err;
    size_t errn;
    int failed;
} J;

static void jfail(J *j, const char *msg)
{
    if (j->failed)
        return;
    j->failed = 1;
    snprintf(j->err, j->errn, "%s at offset %ld", msg, (long)(j->p - j->start));
}

static void ws(J *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int hex4(const char *p, const char *e, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    if (e - p < 4)
        return -1;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int jstring(J *j, gql_str *out)
{
    gql_buf b;
    const char *chunk;
    j->p++;   /* opening quote */
    chunk = j->p;
    buf_init(&b);
    for (;;) {
        unsigned char c;
        if (j->p >= j->e) {
            jfail(j, "unterminated string");
            break;
        }
        c = (unsigned char)*j->p;
        if (c == '"')
            break;
        if (c < 0x20) {
            jfail(j, "control character in string");
            break;
        }
        if (c >= 0x80) {
            int l = utf8_valid_seq((const unsigned char *)j->p, (size_t)(j->e - j->p));
            if (!l) {
                jfail(j, "invalid UTF-8 in string");
                break;
            }
            j->p += l;
            continue;
        }
        if (c == '\\') {
            char ob[4];
            uint32_t cp, lo;
            buf_put(&b, chunk, (size_t)(j->p - chunk));
            j->p++;
            if (j->p >= j->e) {
                jfail(j, "unterminated string");
                break;
            }
            switch (*j->p) {
            case '"': buf_putc(&b, '"'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '/': buf_putc(&b, '/'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'n': buf_putc(&b, '\n'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'u':
                if (hex4(j->p + 1, j->e, &cp)) {
                    jfail(j, "bad unicode escape");
                    goto out;
                }
                j->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (j->e - j->p > 6 && j->p[1] == '\\' && j->p[2] == 'u' &&
                        !hex4(j->p + 3, j->e, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    } else {
                        cp = 0xFFFD;   /* lone surrogate */
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                buf_put(&b, ob, (size_t)utf8_encode(cp, ob));
                break;
            default:
                jfail(j, "bad escape");
                goto out;
            }
            j->p++;
            chunk = j->p;
            continue;
        }
        j->p++;
    }
out:
    if (!j->failed) {
        buf_put(&b, chunk, (size_t)(j->p - chunk));
        j->p++;   /* closing quote */
        out->p = arena_strndup(j->a, b.p ? b.p : "", b.len);
        out->n = (uint32_t)b.len;
        if (!out->p || b.oom)
            jfail(j, "out of memory");
    }
    buf_free(&b);
    return j->failed ? -1 : 0;
}

static gql_json *jvalue(J *j);

static gql_json *jnew(J *j, gql_json_kind k)
{
    gql_json *v = arena_alloc(j->a, sizeof *v);
    if (!v)
        jfail(j, "out of memory");
    else
        v->kind = k;
    return v;
}

static gql_json *jvalue_inner(J *j)
{
    gql_json *v;
    ws(j);
    if (j->p >= j->e) {
        jfail(j, "unexpected end of input");
        return NULL;
    }
    switch (*j->p) {
    case '{': {
        gql_json **tail;
        if (!(v = jnew(j, JS_OBJECT)))
            return NULL;
        tail = &v->first;
        j->p++;
        ws(j);
        if (j->p < j->e && *j->p == '}') {
            j->p++;
            return v;
        }
        for (;;) {
            gql_str key;
            gql_json *m;
            ws(j);
            if (j->p >= j->e || *j->p != '"') {
                jfail(j, "expected object key");
                return NULL;
            }
            if (jstring(j, &key))
                return NULL;
            ws(j);
            if (j->p >= j->e || *j->p != ':') {
                jfail(j, "expected ':'");
                return NULL;
            }
            j->p++;
            if (!(m = jvalue(j)))
                return NULL;
            m->key = key;
            *tail = m;
            tail = &m->next;
            v->count++;
            ws(j);
            if (j->p < j->e && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->e && *j->p == '}') {
                j->p++;
                return v;
            }
            jfail(j, "expected ',' or '}'");
            return NULL;
        }
    }
    case '[': {
        gql_json **tail;
        if (!(v = jnew(j, JS_ARRAY)))
            return NULL;
        tail = &v->first;
        j->p++;
        ws(j);
        if (j->p < j->e && *j->p == ']') {
            j->p++;
            return v;
        }
        for (;;) {
            gql_json *m = jvalue(j);
            if (!m)
                return NULL;
            *tail = m;
            tail = &m->next;
            v->count++;
            ws(j);
            if (j->p < j->e && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->e && *j->p == ']') {
                j->p++;
                return v;
            }
            jfail(j, "expected ',' or ']'");
            return NULL;
        }
    }
    case '"':
        if (!(v = jnew(j, JS_STRING)))
            return NULL;
        if (jstring(j, &v->s))
            return NULL;
        return v;
    case 't':
        if (j->e - j->p >= 4 && memcmp(j->p, "true", 4) == 0) {
            j->p += 4;
            if ((v = jnew(j, JS_BOOL)))
                v->boolean = 1;
            return v;
        }
        break;
    case 'f':
        if (j->e - j->p >= 5 && memcmp(j->p, "false", 5) == 0) {
            j->p += 5;
            return jnew(j, JS_BOOL);
        }
        break;
    case 'n':
        if (j->e - j->p >= 4 && memcmp(j->p, "null", 4) == 0) {
            j->p += 4;
            return jnew(j, JS_NULL);
        }
        break;
    default:
        if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
            const char *s = j->p;
            if (*j->p == '-')
                j->p++;
            if (j->p < j->e && *j->p == '0') {
                j->p++;
            } else if (j->p < j->e && *j->p >= '1' && *j->p <= '9') {
                while (j->p < j->e && *j->p >= '0' && *j->p <= '9')
                    j->p++;
            } else {
                jfail(j, "bad number");
                return NULL;
            }
            if (j->p < j->e && *j->p == '.') {
                j->p++;
                if (!(j->p < j->e && *j->p >= '0' && *j->p <= '9')) {
                    jfail(j, "bad number");
                    return NULL;
                }
                while (j->p < j->e && *j->p >= '0' && *j->p <= '9')
                    j->p++;
            }
            if (j->p < j->e && (*j->p == 'e' || *j->p == 'E')) {
                j->p++;
                if (j->p < j->e && (*j->p == '+' || *j->p == '-'))
                    j->p++;
                if (!(j->p < j->e && *j->p >= '0' && *j->p <= '9')) {
                    jfail(j, "bad number");
                    return NULL;
                }
                while (j->p < j->e && *j->p >= '0' && *j->p <= '9')
                    j->p++;
            }
            if (!(v = jnew(j, JS_NUMBER)))
                return NULL;
            v->s.p = s;
            v->s.n = (uint32_t)(j->p - s);
            return v;
        }
    }
    jfail(j, "unexpected character");
    return NULL;
}

static gql_json *jvalue(J *j)
{
    gql_json *v;
    if (++j->depth > j->max_depth) {
        jfail(j, "maximum nesting depth exceeded");
        return NULL;
    }
    v = jvalue_inner(j);
    j->depth--;
    return j->failed ? NULL : v;
}

gql_json *gql_json_parse(const char *s, size_t n, gql_arena *a, uint32_t max_depth,
                         char *err, size_t errn)
{
    J j;
    gql_json *v;
    memset(&j, 0, sizeof j);
    j.p = j.start = s;
    j.e = s + n;
    j.a = a;
    j.max_depth = max_depth ? max_depth : 128;
    j.err = err;
    j.errn = errn;
    v = jvalue(&j);
    if (v) {
        ws(&j);
        if (j.p != j.e) {
            jfail(&j, "trailing characters");
            v = NULL;
        }
    }
    return j.failed ? NULL : v;
}

void gql_json_write(gql_buf *b, const gql_json *v)
{
    const gql_json *m;
    switch (v->kind) {
    case JS_NULL: buf_puts(b, "null"); break;
    case JS_BOOL: buf_puts(b, v->boolean ? "true" : "false"); break;
    case JS_NUMBER: buf_put(b, v->s.p, v->s.n); break;
    case JS_STRING: buf_json_str(b, v->s.p, v->s.n); break;
    case JS_ARRAY:
        buf_putc(b, '[');
        for (m = v->first; m; m = m->next) {
            if (m != v->first)
                buf_putc(b, ',');
            gql_json_write(b, m);
        }
        buf_putc(b, ']');
        break;
    case JS_OBJECT:
        buf_putc(b, '{');
        for (m = v->first; m; m = m->next) {
            if (m != v->first)
                buf_putc(b, ',');
            buf_json_str(b, m->key.p, m->key.n);
            buf_putc(b, ':');
            gql_json_write(b, m);
        }
        buf_putc(b, '}');
        break;
    }
}

const gql_json *gql_json_get(const gql_json *obj, const char *key, uint32_t n)
{
    const gql_json *m;
    if (!obj || obj->kind != JS_OBJECT)
        return NULL;
    /* last occurrence wins, like JSON.parse */
    {
        const gql_json *found = NULL;
        for (m = obj->first; m; m = m->next)
            if (m->key.n == n && memcmp(m->key.p, key, n) == 0)
                found = m;
        return found;
    }
}
