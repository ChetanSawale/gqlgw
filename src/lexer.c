/* lexer.c - GraphQL lexer. */
#include "lexer.h"

#include <stdio.h>
#include <string.h>

void lex_init(gql_lexer *lx, const char *src, size_t n, gql_arena *a, uint32_t max_tokens)
{
    memset(lx, 0, sizeof *lx);
    lx->src = lx->pos = src;
    lx->end = src + n;
    lx->line = 1;
    lx->line_start = lx->cpos = src;
    lx->ccol = 1;
    lx->arena = a;
    lx->max_tokens = max_tokens;
    lx->tok.kind = T_SOF;
}

/* column of p (on current line) in UTF-16 code units, 1-based, incremental */
static uint32_t col_of(gql_lexer *lx, const char *p)
{
    if (lx->cpos < lx->line_start || lx->cpos > p) {
        lx->cpos = lx->line_start;
        lx->ccol = 1;
    }
    while (lx->cpos < p) {
        unsigned char c = (unsigned char)*lx->cpos;
        if (c < 0x80 || (c & 0xC0) == 0xC0)
            lx->ccol += (c >= 0xF0) ? 2 : 1;   /* astral code point = 2 UTF-16 units */
        lx->cpos++;
    }
    return lx->ccol;
}

static gql_loc loc_of(gql_lexer *lx, const char *p)
{
    gql_loc l;
    l.line = lx->line;
    l.col = col_of(lx, p);
    return l;
}

static int lex_error(gql_lexer *lx, const char *at, const char *fmt, ...) GQL_PRINTF(3, 4);
static int lex_error(gql_lexer *lx, const char *at, const char *fmt, ...)
{
    va_list ap;
    int n;
    lx->failed = 1;
    lx->err_loc = loc_of(lx, at);
    n = snprintf(lx->err, sizeof lx->err, "Syntax Error: ");
    va_start(ap, fmt);
    vsnprintf(lx->err + n, sizeof lx->err - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}

static void newline(gql_lexer *lx, const char *after)
{
    lx->line++;
    lx->line_start = after;
    lx->cpos = after;
    lx->ccol = 1;
}

/* describe the (possibly multi-byte) character at p for error messages */
static void char_desc(const char *p, const char *end, char *out, size_t n)
{
    unsigned char c = (unsigned char)*p;
    if (p >= end) {
        snprintf(out, n, "<EOF>");
    } else if (c >= 0x20 && c < 0x7F) {
        if (c == '"')
            snprintf(out, n, "'\"'");
        else
            snprintf(out, n, "\"%c\"", c);
    } else if (c < 0x80) {
        snprintf(out, n, "U+%04X", c);
    } else {
        int len = utf8_valid_seq((const unsigned char *)p, (size_t)(end - p));
        if (len > 0 && (size_t)len + 3 < n)
            snprintf(out, n, "\"%.*s\"", len, p);
        else
            snprintf(out, n, "U+FFFD");
    }
}

static inline int is_name_start(int c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline int is_name_cont(int c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static inline int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int read_number(gql_lexer *lx, const char *start)
{
    const char *p = start, *e = lx->end;
    char d[16];
    int is_float = 0;

    if (p < e && *p == '-')
        p++;
    if (p < e && *p == '0') {
        p++;
        if (p < e && is_digit((unsigned char)*p)) {
            char_desc(p, e, d, sizeof d);
            return lex_error(lx, p, "Invalid number, unexpected digit after 0: %s.", d);
        }
    } else if (p < e && is_digit((unsigned char)*p)) {
        while (p < e && is_digit((unsigned char)*p))
            p++;
    } else {
        char_desc(p, e, d, sizeof d);
        return lex_error(lx, p, "Invalid number, expected digit but got: %s.", d);
    }
    if (p < e && *p == '.') {
        is_float = 1;
        p++;
        if (!(p < e && is_digit((unsigned char)*p))) {
            char_desc(p, e, d, sizeof d);
            return lex_error(lx, p, "Invalid number, expected digit but got: %s.", d);
        }
        while (p < e && is_digit((unsigned char)*p))
            p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        is_float = 1;
        p++;
        if (p < e && (*p == '+' || *p == '-'))
            p++;
        if (!(p < e && is_digit((unsigned char)*p))) {
            char_desc(p, e, d, sizeof d);
            return lex_error(lx, p, "Invalid number, expected digit but got: %s.", d);
        }
        while (p < e && is_digit((unsigned char)*p))
            p++;
    }
    if (p < e && (*p == '.' || is_name_start((unsigned char)*p))) {
        char_desc(p, e, d, sizeof d);
        return lex_error(lx, p, "Invalid number, expected digit but got: %s.", d);
    }
    lx->tok.kind = is_float ? T_FLOAT : T_INT;
    lx->tok.len = (uint32_t)(p - start);
    lx->tok.val.p = start;
    lx->tok.val.n = lx->tok.len;
    lx->pos = p;
    return 0;
}

/* read \uXXXX or \u{X...}; p points after "\u"; returns code point or -1 */
static long read_unicode_escape(const char **pp, const char *e)
{
    const char *p = *pp;
    long cp = 0;
    int i, h;
    if (p < e && *p == '{') {
        p++;
        for (i = 0; p < e && *p != '}'; i++, p++) {
            if ((h = hexval((unsigned char)*p)) < 0 || i >= 8)
                return -1;
            cp = cp * 16 + h;
            if (cp > 0x10FFFF)
                return -1;
        }
        if (p >= e || i == 0)
            return -1;
        *pp = p + 1;
        return cp;
    }
    for (i = 0; i < 4; i++) {
        if (p + i >= e || (h = hexval((unsigned char)p[i])) < 0)
            return -1;
        cp = cp * 16 + h;
    }
    *pp = p + 4;
    return cp;
}

static int read_string(gql_lexer *lx, const char *start)
{
    const char *p = start + 1, *e = lx->end, *chunk = p;
    gql_buf b;
    char d[16];

    buf_init(&b);
    for (;;) {
        unsigned char c;
        if (p >= e) {
            buf_free(&b);
            return lex_error(lx, p, "Unterminated string.");
        }
        c = (unsigned char)*p;
        if (c == '"')
            break;
        if (c == '\n' || c == '\r') {
            buf_free(&b);
            return lex_error(lx, p, "Unterminated string.");
        }
        if (c < 0x20 && c != '\t') {
            buf_free(&b);
            return lex_error(lx, p, "Invalid character within String: U+%04X.", c);
        }
        if (c >= 0x80) {
            int len = utf8_valid_seq((const unsigned char *)p, (size_t)(e - p));
            if (!len) {
                buf_free(&b);
                return lex_error(lx, p, "Invalid UTF-8 sequence within String.");
            }
            p += len;
            continue;
        }
        if (c == '\\') {
            const char *esc = p;
            char out[4];
            long cp;
            buf_put(&b, chunk, (size_t)(p - chunk));
            p++;
            if (p >= e) {
                buf_free(&b);
                return lex_error(lx, p, "Unterminated string.");
            }
            switch (*p) {
            case '"': buf_putc(&b, '"'); p++; break;
            case '\\': buf_putc(&b, '\\'); p++; break;
            case '/': buf_putc(&b, '/'); p++; break;
            case 'b': buf_putc(&b, '\b'); p++; break;
            case 'f': buf_putc(&b, '\f'); p++; break;
            case 'n': buf_putc(&b, '\n'); p++; break;
            case 'r': buf_putc(&b, '\r'); p++; break;
            case 't': buf_putc(&b, '\t'); p++; break;
            case 'u':
                p++;
                cp = read_unicode_escape(&p, e);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* high surrogate: must be followed by \uDC00-\uDFFF */
                    const char *q = p;
                    long lo = -1;
                    if (q + 1 < e && q[0] == '\\' && q[1] == 'u') {
                        q += 2;
                        lo = read_unicode_escape(&q, e);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p = q;
                    } else {
                        cp = -1;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = -1;
                }
                if (cp < 0) {
                    const char *ee = p > esc + 2 ? p : esc + 2;
                    if (ee - esc > 12)
                        ee = esc + 12;
                    if (ee > e)
                        ee = e;
                    buf_free(&b);
                    return lex_error(lx, esc, "Invalid Unicode escape sequence: \"%.*s\".",
                                     (int)(ee - esc), esc);
                }
                buf_put(&b, out, (size_t)utf8_encode((uint32_t)cp, out));
                break;
            default:
                char_desc(p, e, d, sizeof d);
                buf_free(&b);
                return lex_error(lx, esc, "Invalid character escape sequence: \\%s.", d);
            }
            chunk = p;
            continue;
        }
        p++;
    }
    buf_put(&b, chunk, (size_t)(p - chunk));
    if (b.oom) {
        buf_free(&b);
        return lex_error(lx, start, "Out of memory.");
    }
    lx->tok.kind = T_STRING;
    lx->tok.len = (uint32_t)(p + 1 - start);
    lx->tok.val.p = arena_strndup(lx->arena, b.p ? b.p : "", b.len);
    lx->tok.val.n = (uint32_t)b.len;
    buf_free(&b);
    if (!lx->tok.val.p)
        return lex_error(lx, start, "Out of memory.");
    lx->pos = p + 1;
    return 0;
}

/* spec BlockStringValue(rawValue) */
static gql_str block_string_value(gql_arena *a, const char *raw, size_t n, int *oom)
{
    gql_str out = { "", 0 };
    const char *p, *e = raw + n;
    size_t common = (size_t)-1, nlines = 0, i, first_nonblank = (size_t)-1, last_nonblank = 0;
    gql_buf b;

    /* pass 1: compute common indent and blank line bounds */
    p = raw;
    for (i = 0;; i++) {
        const char *ls = p, *le;
        size_t indent = 0;
        while (p < e && *p != '\n' && *p != '\r')
            p++;
        le = p;
        while (ls + indent < le && (ls[indent] == ' ' || ls[indent] == '\t'))
            indent++;
        if (ls + indent < le) {   /* non-blank line */
            if (i > 0 && indent < common)
                common = indent;
            if (first_nonblank == (size_t)-1)
                first_nonblank = i;
            last_nonblank = i;
        }
        nlines++;
        if (p >= e)
            break;
        if (*p == '\r' && p + 1 < e && p[1] == '\n')
            p += 2;
        else
            p++;
    }
    if (first_nonblank == (size_t)-1)
        return out;   /* only blank lines */
    if (common == (size_t)-1)
        common = 0;

    /* pass 2: emit lines [first_nonblank, last_nonblank] with indent removed (except line 0) */
    buf_init(&b);
    p = raw;
    for (i = 0; i < nlines; i++) {
        const char *ls = p, *le;
        while (p < e && *p != '\n' && *p != '\r')
            p++;
        le = p;
        if (i >= first_nonblank && i <= last_nonblank) {
            if (i > first_nonblank)
                buf_putc(&b, '\n');
            if (i > 0) {
                size_t len = (size_t)(le - ls);
                ls += common < len ? common : len;
            }
            buf_put(&b, ls, (size_t)(le - ls));
        }
        if (p < e) {
            if (*p == '\r' && p + 1 < e && p[1] == '\n')
                p += 2;
            else
                p++;
        }
    }
    if (b.oom) {
        *oom = 1;
    } else {
        out.p = arena_strndup(a, b.p ? b.p : "", b.len);
        out.n = (uint32_t)b.len;
        if (!out.p)
            *oom = 1;
    }
    buf_free(&b);
    return out;
}

static int read_block_string(gql_lexer *lx, const char *start)
{
    const char *p = start + 3, *e = lx->end;
    gql_buf raw;
    const char *chunk = p;
    int oom = 0;

    buf_init(&raw);
    for (;;) {
        unsigned char c;
        if (p >= e) {
            buf_free(&raw);
            return lex_error(lx, p, "Unterminated string.");
        }
        c = (unsigned char)*p;
        if (c == '"' && p + 2 < e && p[1] == '"' && p[2] == '"')
            break;
        if (c == '\\' && p + 3 < e && p[1] == '"' && p[2] == '"' && p[3] == '"') {
            buf_put(&raw, chunk, (size_t)(p - chunk));
            buf_puts(&raw, "\"\"\"");
            p += 4;
            chunk = p;
            continue;
        }
        if (c == '\n') {
            p++;
            newline(lx, p);
            continue;
        }
        if (c == '\r') {
            p += (p + 1 < e && p[1] == '\n') ? 2 : 1;
            newline(lx, p);
            continue;
        }
        if (c < 0x20 && c != '\t') {
            buf_free(&raw);
            return lex_error(lx, p, "Invalid character within String: U+%04X.", c);
        }
        if (c >= 0x80) {
            int len = utf8_valid_seq((const unsigned char *)p, (size_t)(e - p));
            if (!len) {
                buf_free(&raw);
                return lex_error(lx, p, "Invalid UTF-8 sequence within String.");
            }
            p += len;
            continue;
        }
        p++;
    }
    buf_put(&raw, chunk, (size_t)(p - chunk));
    lx->tok.kind = T_BLOCK_STRING;
    lx->tok.len = (uint32_t)(p + 3 - start);
    lx->tok.val = block_string_value(lx->arena, raw.p ? raw.p : "", raw.len, &oom);
    buf_free(&raw);
    if (oom)
        return lex_error(lx, start, "Out of memory.");
    lx->pos = p + 3;
    return 0;
}

int lex_next(gql_lexer *lx)
{
    const char *p, *e = lx->end;
    gql_tok_kind k;
    char d[16];

    if (lx->failed)
        return -1;
    if (lx->tok.kind == T_EOF)
        return 0;
    p = lx->pos;
    /* skip ignored tokens */
    for (;;) {
        unsigned char c;
        if (p >= e)
            break;
        c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == ',') {
            p++;
        } else if (c == '\n') {
            p++;
            newline(lx, p);
        } else if (c == '\r') {
            p += (p + 1 < e && p[1] == '\n') ? 2 : 1;
            newline(lx, p);
        } else if (c == 0xEF && p + 2 < e && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
            p += 3;   /* BOM */
        } else if (c == '#') {
            while (p < e && *p != '\n' && *p != '\r')
                p++;
        } else {
            break;
        }
    }
    lx->pos = p;
    lx->tok.start = p;
    lx->tok.loc = loc_of(lx, p);
    lx->tok.val.p = p;
    lx->tok.val.n = 0;

    if (++lx->ntokens > lx->max_tokens && lx->max_tokens)
        return lex_error(lx, p, "Document contains more than %u tokens. Parsing aborted.", lx->max_tokens);

    if (p >= e) {
        lx->tok.kind = T_EOF;
        lx->tok.len = 0;
        return 0;
    }
    switch (*p) {
    case '!': k = T_BANG; break;
    case '$': k = T_DOLLAR; break;
    case '&': k = T_AMP; break;
    case '(': k = T_LPAREN; break;
    case ')': k = T_RPAREN; break;
    case ':': k = T_COLON; break;
    case '=': k = T_EQUALS; break;
    case '@': k = T_AT; break;
    case '[': k = T_LBRACKET; break;
    case ']': k = T_RBRACKET; break;
    case '{': k = T_LBRACE; break;
    case '|': k = T_PIPE; break;
    case '}': k = T_RBRACE; break;
    case '.':
        if (p + 2 < e && p[1] == '.' && p[2] == '.') {
            lx->tok.kind = T_SPREAD;
            lx->tok.len = 3;
            lx->pos = p + 3;
            return 0;
        }
        return lex_error(lx, p, "Unexpected character: \".\".");
    case '"':
        if (p + 2 < e && p[1] == '"' && p[2] == '"')
            return read_block_string(lx, p);
        return read_string(lx, p);
    default:
        if (is_name_start((unsigned char)*p)) {
            const char *s = p++;
            while (p < e && is_name_cont((unsigned char)*p))
                p++;
            lx->tok.kind = T_NAME;
            lx->tok.len = (uint32_t)(p - s);
            lx->tok.val.p = s;
            lx->tok.val.n = lx->tok.len;
            lx->pos = p;
            return 0;
        }
        if (*p == '-' || is_digit((unsigned char)*p))
            return read_number(lx, p);
        char_desc(p, e, d, sizeof d);
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7F)
            return lex_error(lx, p, "Invalid character: %s.", d);
        return lex_error(lx, p, "Unexpected character: %s.", d);
    }
    lx->tok.kind = k;
    lx->tok.len = 1;
    lx->pos = p + 1;
    return 0;
}

const char *tok_kind_desc(gql_tok_kind k)
{
    switch (k) {
    case T_SOF: return "<SOF>";
    case T_EOF: return "<EOF>";
    case T_BANG: return "\"!\"";
    case T_DOLLAR: return "\"$\"";
    case T_AMP: return "\"&\"";
    case T_LPAREN: return "\"(\"";
    case T_RPAREN: return "\")\"";
    case T_SPREAD: return "\"...\"";
    case T_COLON: return "\":\"";
    case T_EQUALS: return "\"=\"";
    case T_AT: return "\"@\"";
    case T_LBRACKET: return "\"[\"";
    case T_RBRACKET: return "\"]\"";
    case T_LBRACE: return "\"{\"";
    case T_PIPE: return "\"|\"";
    case T_RBRACE: return "\"}\"";
    case T_NAME: return "Name";
    case T_INT: return "Int";
    case T_FLOAT: return "Float";
    case T_STRING: return "String";
    case T_BLOCK_STRING: return "BlockString";
    }
    return "?";
}

void lex_describe(const gql_token *t, char *out, size_t outn)
{
    switch (t->kind) {
    case T_NAME: case T_INT: case T_FLOAT:
        snprintf(out, outn, "%s \"%.*s\"", tok_kind_desc(t->kind),
                 (int)(t->len > 40 ? 40 : t->len), t->start);
        break;
    case T_STRING: case T_BLOCK_STRING:
        snprintf(out, outn, "%s \"%.*s\"", tok_kind_desc(t->kind), (int)(t->val.n > 200 ? 200 : t->val.n), t->val.p);
        break;
    default:
        snprintf(out, outn, "%s", tok_kind_desc(t->kind));
    }
}
