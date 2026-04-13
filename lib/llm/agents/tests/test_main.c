#include "test_harness.h"

/* Global test counters (declared extern in test_harness.h) */
int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

extern void run_side_agent_tests(void);
extern void run_mem_agent_tests(void);

int main(void)
{
    run_side_agent_tests();
    run_mem_agent_tests();
    return TEST_SUMMARY();
}
