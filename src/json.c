#include "json.h"

#include <math.h>
#include <string.h>

void ua_json_init(ua_json *j, FILE *f)
{
    memset(j, 0, sizeof *j);
    j->f = f;
}

int ua_json_finish(ua_json *j)
{
    if (j->depth != 0 || j->after_key)
        j->error = 1;
    if (!j->error)
        fputc('\n', j->f);
    if (ferror(j->f))
        j->error = 1;
    return j->error ? -1 : 0;
}

/* Called before every value: emits the separating comma and newline. */
static void pre_value(ua_json *j)
{
    if (j->after_key) {
        j->after_key = 0;
        return;
    }
    if (j->depth > 0) {
        if (j->count[j->depth]++)
            fputc(',', j->f);
        fputc('\n', j->f);
        for (int i = 0; i < j->depth; i++)
            fputc(' ', j->f);
    }
}

static void j_open(ua_json *j, char c)
{
    pre_value(j);
    if (j->depth + 1 >= UA_JSON_MAX_DEPTH) {
        j->error = 1;
        return;
    }
    fputc(c, j->f);
    j->depth++;
    j->count[j->depth] = 0;
}

static void j_close(ua_json *j, char c)
{
    if (j->depth <= 0 || j->after_key) {
        j->error = 1;
        return;
    }
    int had = j->count[j->depth];
    j->depth--;
    if (had) {
        fputc('\n', j->f);
        for (int i = 0; i < j->depth; i++)
            fputc(' ', j->f);
    }
    fputc(c, j->f);
}

void ua_json_begin_object(ua_json *j) { j_open(j, '{'); }
void ua_json_end_object(ua_json *j) { j_close(j, '}'); }
void ua_json_begin_array(ua_json *j) { j_open(j, '['); }
void ua_json_end_array(ua_json *j) { j_close(j, ']'); }

static void put_string(ua_json *j, const char *s)
{
    fputc('"', j->f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", j->f); break;
        case '\\': fputs("\\\\", j->f); break;
        case '\n': fputs("\\n", j->f); break;
        case '\r': fputs("\\r", j->f); break;
        case '\t': fputs("\\t", j->f); break;
        default:
            if (*p < 0x20)
                fprintf(j->f, "\\u%04x", *p);
            else
                fputc(*p, j->f);
        }
    }
    fputc('"', j->f);
}

void ua_json_key(ua_json *j, const char *key)
{
    if (j->after_key || j->depth == 0) {
        j->error = 1;
        return;
    }
    pre_value(j);
    put_string(j, key);
    fputs(": ", j->f);
    j->after_key = 1;
}

void ua_json_string(ua_json *j, const char *s)
{
    pre_value(j);
    put_string(j, s);
}

void ua_json_int(ua_json *j, long long v)
{
    pre_value(j);
    fprintf(j->f, "%lld", v);
}

void ua_json_uint(ua_json *j, unsigned long long v)
{
    pre_value(j);
    fprintf(j->f, "%llu", v);
}

void ua_json_number(ua_json *j, double v, int decimals)
{
    char buf[64];
    pre_value(j);
    if (!isfinite(v)) {
        fputs("null", j->f);
        return;
    }
    if (decimals < 0)
        decimals = 0;
    if (decimals > 12)
        decimals = 12;
    snprintf(buf, sizeof buf, "%.*f", decimals, v);
    if (strchr(buf, '.')) {
        size_t n = strlen(buf);
        while (n > 0 && buf[n - 1] == '0')
            buf[--n] = 0;
        if (n > 0 && buf[n - 1] == '.')
            buf[--n] = 0;
    }
    if (strcmp(buf, "-0") == 0)
        strcpy(buf, "0");
    fputs(buf, j->f);
}

void ua_json_bool(ua_json *j, int v)
{
    pre_value(j);
    fputs(v ? "true" : "false", j->f);
}

void ua_json_null(ua_json *j)
{
    pre_value(j);
    fputs("null", j->f);
}

void ua_json_kstring(ua_json *j, const char *key, const char *s)
{
    ua_json_key(j, key);
    ua_json_string(j, s);
}

void ua_json_kint(ua_json *j, const char *key, long long v)
{
    ua_json_key(j, key);
    ua_json_int(j, v);
}

void ua_json_kuint(ua_json *j, const char *key, unsigned long long v)
{
    ua_json_key(j, key);
    ua_json_uint(j, v);
}

void ua_json_knumber(ua_json *j, const char *key, double v, int decimals)
{
    ua_json_key(j, key);
    ua_json_number(j, v, decimals);
}

void ua_json_kbool(ua_json *j, const char *key, int v)
{
    ua_json_key(j, key);
    ua_json_bool(j, v);
}
