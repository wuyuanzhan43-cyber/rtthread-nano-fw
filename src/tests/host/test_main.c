/*
 * test_main.c - Host 测试入口。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 定义全局失败计数；
 *   2. 依次运行 framework 与 app_rpc 两个测试套件；
 *   3. 任一 CHECK 失败时返回非 0，供 CTest 判定。
 *
 * 上下文：单线程。
 * 依赖：test_common.h。
 */
#include <stdio.h>
#include "test_common.h"

/* 所有测试文件共享的失败计数。 */
int test_failures;

void test_framework_run(void);
void test_task_run(void);
void test_app_rpc_run(void);
void test_peripherals_run(void);

int main(void)
{
    test_framework_run();
    test_task_run();
    test_app_rpc_run();
    test_peripherals_run();

    if (test_failures != 0) {
        printf("%d test(s) failed\n", test_failures);
        return 1;
    }
    printf("all v2 host tests passed\n");
    return 0;
}
