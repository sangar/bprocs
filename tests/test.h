#pragma once

#include "mc/core/arena.h"
#include "mc/text/str.h"

// Test is one case's state: a fresh arena and its tally of checks.
typedef struct {
    const char *name;
    Arena *arena;
    unsigned checks;
    unsigned failures;
} Test;

// test_check records one assertion; what is printed on failure.
void test_check(Test *test, bool ok, const char *what);
void test_check_str(Test *test, String actual, String expected, const char *what);

typedef void (*TestFunction)(Test *test);

typedef struct {
    const char *name;
    TestFunction run;
} TestCase;

// Each test file exports one table, ended by a { nullptr, nullptr } entry.
extern const TestCase INPUT_TESTS[];
extern const TestCase LOG_TESTS[];
extern const TestCase CONFIG_TESTS[];
extern const TestCase SCREEN_TESTS[];
extern const TestCase PROC_TESTS[];
extern const TestCase CHECK_TESTS[];
extern const TestCase SUPERVISOR_TESTS[];
extern const TestCase SERVER_TESTS[];
