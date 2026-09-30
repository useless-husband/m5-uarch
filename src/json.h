/*
 * json.h - a minimal streaming JSON writer.
 * Commas and nesting are tracked here so that callers cannot emit invalid
 * documents; non-finite numbers become null.
 */
#ifndef UA_JSON_H
#define UA_JSON_H

#include <stdio.h>

#define UA_JSON_MAX_DEPTH 32

typedef struct {
    FILE *f;
    int depth;
    int count[UA_JSON_MAX_DEPTH]; /* values written at each level */
    int after_key;
    int error;                    /* set on misuse or I/O failure */
} ua_json;

void ua_json_init(ua_json *j, FILE *f);
/* Returns 0 if the document is complete and no error occurred. */
int ua_json_finish(ua_json *j);

void ua_json_begin_object(ua_json *j);
void ua_json_end_object(ua_json *j);
void ua_json_begin_array(ua_json *j);
void ua_json_end_array(ua_json *j);

void ua_json_key(ua_json *j, const char *key);
void ua_json_string(ua_json *j, const char *s);
void ua_json_int(ua_json *j, long long v);
void ua_json_uint(ua_json *j, unsigned long long v);
/* `decimals` digits after the point; trailing zeros are trimmed. */
void ua_json_number(ua_json *j, double v, int decimals);
void ua_json_bool(ua_json *j, int v);
void ua_json_null(ua_json *j);

/* key + value helpers */
void ua_json_kstring(ua_json *j, const char *key, const char *s);
void ua_json_kint(ua_json *j, const char *key, long long v);
void ua_json_kuint(ua_json *j, const char *key, unsigned long long v);
void ua_json_knumber(ua_json *j, const char *key, double v, int decimals);
void ua_json_kbool(ua_json *j, const char *key, int v);

#endif
