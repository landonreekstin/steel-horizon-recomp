#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int failed;
} parser;

static jnode *new_node(jtype t)
{
    jnode *n = calloc(1, sizeof *n);
    n->type = t;
    return n;
}

static void skip_ws(parser *ps)
{
    while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++;
}

static void fail(parser *ps, const char *what)
{
    if (!ps->failed) fprintf(stderr, "json: %s near '%.20s'\n", what, ps->p);
    ps->failed = 1;
}

static char *parse_string_raw(parser *ps)
{
    if (*ps->p != '"') { fail(ps, "expected string"); return NULL; }
    ps->p++;
    size_t cap = 16, len = 0;
    char *out = malloc(cap);
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p++;
        if (c == '\\') {
            char e = *ps->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {                      /* keep ASCII, replace the rest */
                unsigned v = 0;
                for (int i = 0; i < 4 && *ps->p; i++, ps->p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*ps->p) ? *ps->p - '0'
                                                       : (tolower((unsigned char)*ps->p) - 'a' + 10));
                c = v < 128 ? (char)v : '?';
                break;
            }
            default: c = e; break;           /* \" \\ \/ */
            }
        }
        if (len + 2 > cap) out = realloc(out, cap *= 2);
        out[len++] = c;
    }
    if (*ps->p != '"') { fail(ps, "unterminated string"); free(out); return NULL; }
    ps->p++;
    out[len] = 0;
    return out;
}

static jnode *parse_value(parser *ps);

static jnode *parse_container(parser *ps, int is_obj)
{
    jnode *n = new_node(is_obj ? J_OBJ : J_ARR), **tail = &n->child;
    char close = is_obj ? '}' : ']';
    ps->p++;
    skip_ws(ps);
    if (*ps->p == close) { ps->p++; return n; }
    while (!ps->failed) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            key = parse_string_raw(ps);
            skip_ws(ps);
            if (*ps->p != ':') { fail(ps, "expected ':'"); free(key); break; }
            ps->p++;
        }
        jnode *v = parse_value(ps);
        if (!v) { free(key); break; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return n; }
        fail(ps, "expected ',' or closing bracket");
    }
    json_free(n);
    return NULL;
}

static jnode *parse_value(parser *ps)
{
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{' || c == '[') return parse_container(ps, c == '{');
    if (c == '"') {
        jnode *n = new_node(J_STR);
        n->str = parse_string_raw(ps);
        if (!n->str) { free(n); return NULL; }
        return n;
    }
    if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
        jnode *n = new_node(J_BOOL);
        n->num = c == 't';
        ps->p += c == 't' ? 4 : 5;
        return n;
    }
    if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return new_node(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        jnode *n = new_node(J_NUM);
        n->num = strtod(ps->p, &end);
        ps->p = end;
        return n;
    }
    fail(ps, "unexpected character");
    return NULL;
}

jnode *json_parse(const char *text)
{
    parser ps = {text, 0};
    jnode *n = parse_value(&ps);
    if (n && !ps.failed) {
        skip_ws(&ps);
        if (*ps.p) { fail(&ps, "trailing data"); json_free(n); return NULL; }
    }
    return ps.failed ? NULL : n;
}

jnode *json_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    jnode *n = json_parse(buf);
    free(buf);
    return n;
}

void json_free(jnode *n)
{
    while (n) {
        jnode *next = n->next;
        json_free(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}

jnode *json_get(const jnode *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (jnode *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

double json_num(const jnode *obj, const char *key, double fallback)
{
    jnode *n = json_get(obj, key);
    return n && (n->type == J_NUM || n->type == J_BOOL) ? n->num : fallback;
}

const char *json_str(const jnode *obj, const char *key)
{
    jnode *n = json_get(obj, key);
    return n && n->type == J_STR ? n->str : NULL;
}

size_t json_len(const jnode *arr)
{
    size_t n = 0;
    for (jnode *c = arr ? arr->child : NULL; c; c = c->next) n++;
    return n;
}
