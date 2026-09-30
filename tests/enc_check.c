/*
 * enc_check.c - second half of the encoder cross-check: compare the words
 * produced by src/enc.h with what the system assembler made of the same
 * instructions (see enc_gen.c).
 */
#include "check.h"

#include <stdint.h>

extern const uint32_t enc_ref[], enc_ref_end[];

static const struct {
    uint32_t word;
    const char *text;
} cases[] = {
#include "enc_cases.inc"
};

int main(void)
{
    size_t n = sizeof cases / sizeof cases[0];
    CHECK((size_t)(enc_ref_end - enc_ref) == n);
    if (g_failures)
        return test_finish("test_enc");
    for (size_t i = 0; i < n; i++) {
        g_checks++;
        if (cases[i].word != enc_ref[i]) {
            g_failures++;
            if (g_failures <= 20)
                fprintf(stderr, "encoder %08x, assembler %08x: %s\n", cases[i].word, enc_ref[i],
                        cases[i].text);
        }
    }
    return test_finish("test_enc");
}
