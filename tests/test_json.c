/* Tests for src/json.c: exact output for small documents, escaping,
 * non-finite numbers, and detection of misuse. */
#include "check.h"
#include "json.h"

#include <string.h>

static char g_buf[4096];

static FILE *open_buf(void)
{
    memset(g_buf, 0, sizeof g_buf);
    return fmemopen(g_buf, sizeof g_buf - 1, "w");
}

int main(void)
{
    ua_json j;
    FILE *f = open_buf();
    ua_json_init(&j, f);
    ua_json_begin_object(&j);
    ua_json_kstring(&j, "s", "a\"b\\c\n\t\x01");
    ua_json_kint(&j, "i", -5);
    ua_json_kuint(&j, "u", 18446744073709551615ull);
    ua_json_knumber(&j, "x", 1.500, 3);
    ua_json_knumber(&j, "y", 2.0, 3);
    ua_json_knumber(&j, "z", -0.0001, 2);
    ua_json_knumber(&j, "nan", NAN, 3);
    ua_json_kbool(&j, "t", 1);
    ua_json_key(&j, "a");
    ua_json_begin_array(&j);
    ua_json_int(&j, 1);
    ua_json_begin_array(&j);
    ua_json_end_array(&j);
    ua_json_null(&j);
    ua_json_end_array(&j);
    ua_json_key(&j, "o");
    ua_json_begin_object(&j);
    ua_json_end_object(&j);
    ua_json_end_object(&j);
    CHECK(ua_json_finish(&j) == 0);
    fclose(f);
    const char *want = "{\n"
                       " \"s\": \"a\\\"b\\\\c\\n\\t\\u0001\",\n"
                       " \"i\": -5,\n"
                       " \"u\": 18446744073709551615,\n"
                       " \"x\": 1.5,\n"
                       " \"y\": 2,\n"
                       " \"z\": 0,\n"
                       " \"nan\": null,\n"
                       " \"t\": true,\n"
                       " \"a\": [\n"
                       "  1,\n"
                       "  [],\n"
                       "  null\n"
                       " ],\n"
                       " \"o\": {}\n"
                       "}\n";
    CHECK(strcmp(g_buf, want) == 0);
    if (strcmp(g_buf, want) != 0)
        fprintf(stderr, "got:\n%s\nwant:\n%s\n", g_buf, want);

    /* Misuse is reported instead of producing invalid JSON silently. */
    f = open_buf();
    ua_json_init(&j, f);
    ua_json_begin_object(&j);
    CHECK(ua_json_finish(&j) != 0); /* unterminated */
    fclose(f);

    f = open_buf();
    ua_json_init(&j, f);
    ua_json_begin_object(&j);
    ua_json_key(&j, "a");
    ua_json_key(&j, "b"); /* two keys in a row */
    ua_json_int(&j, 1);
    ua_json_end_object(&j);
    CHECK(ua_json_finish(&j) != 0);
    fclose(f);

    f = open_buf();
    ua_json_init(&j, f);
    ua_json_begin_object(&j);
    ua_json_key(&j, "a");
    ua_json_end_object(&j); /* key without value */
    CHECK(ua_json_finish(&j) != 0);
    fclose(f);

    f = open_buf();
    ua_json_init(&j, f);
    ua_json_end_array(&j); /* close without open */
    CHECK(ua_json_finish(&j) != 0);
    fclose(f);

    f = open_buf();
    ua_json_init(&j, f);
    for (int i = 0; i < UA_JSON_MAX_DEPTH + 4; i++)
        ua_json_begin_array(&j); /* too deep */
    CHECK(j.error);
    fclose(f);

    return test_finish("test_json");
}
