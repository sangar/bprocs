#include "test.h"

#include <stdio.h>

void test_check(Test *test, bool ok, const char *what)
{
    test->checks++;
    if (!ok) {
        test->failures++;
        printf("  FAIL %s: %s\n", test->name, what);
    }
}

void test_check_str(Test *test, String actual, String expected, const char *what)
{
    test->checks++;
    if (!str_equal(actual, expected)) {
        test->failures++;
        printf("  FAIL %s: %s\n       expected \"%.*s\"\n       actual   \"%.*s\"\n", test->name, what,
               (int)expected.len, expected.data, (int)actual.len, actual.data);
    }
}

static unsigned run_table(const TestCase *cases, unsigned *checks)
{
    unsigned failed = 0;
    for (const TestCase *c = cases; c->name != nullptr; c++) {
        Test test = { .name = c->name, .arena = arena_create(1 << 16) };
        c->run(&test);
        arena_destroy(test.arena);
        *checks += test.checks;
        failed += test.failures > 0;
    }
    return failed;
}

int main(void)
{
    const TestCase *tables[] = { INPUT_TESTS, LOG_TESTS,        CONFIG_TESTS, SCREEN_TESTS,
                                 PROC_TESTS,  CHECK_TESTS,      SUPERVISOR_TESTS, SERVER_TESTS };
    unsigned failed = 0;
    unsigned checks = 0;
    for (size_t i = 0; i < countof(tables); i++) {
        failed += run_table(tables[i], &checks);
    }
    if (failed > 0) {
        printf("%u test(s) failed, %u checks\n", failed, checks);
        return 1;
    }
    printf("all tests passed, %u checks\n", checks);
    return 0;
}
