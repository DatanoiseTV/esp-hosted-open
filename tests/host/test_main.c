/*
 * Tiny TAP-ish test runner. Each test is a function returning 0 on
 * pass, non-zero on failure.
 */

#include <stdio.h>

int test_proto_sizes(void);
int test_proto_msgid_namespace(void);
int test_proto_caps_bitmap(void);

typedef int (*test_fn_t)(void);

struct test { const char *name; test_fn_t fn; };

int main(void)
{
    struct test tests[] = {
        { "proto: packed-struct sizes",  test_proto_sizes },
        { "proto: msg_id namespace",     test_proto_msgid_namespace },
        { "proto: caps bitmap encoding", test_proto_caps_bitmap },
    };
    size_t n = sizeof(tests) / sizeof(*tests);
    int failed = 0;
    for (size_t i = 0; i < n; i++) {
        printf("[%zu/%zu] %-44s ", i + 1, n, tests[i].name);
        fflush(stdout);
        int r = tests[i].fn();
        if (r == 0) printf("\033[32mOK\033[0m\n");
        else { printf("\033[31mFAIL\033[0m (rc=%d)\n", r); failed++; }
    }
    printf("\n");
    if (!failed) printf("\033[32mAll %zu tests passed.\033[0m\n", n);
    else         printf("\033[31m%d/%zu tests failed.\033[0m\n", failed, n);
    return failed ? 1 : 0;
}
