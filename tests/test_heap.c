#include "tsched/tsched.h"

#include <stdio.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed: %s\n", #expression); \
        return 1; \
    } \
} while (0)

int main(void)
{
    struct tsched_heap heap;
    struct tsched_task tasks[4] = {{0}};
    tasks[0].next_run_ms = 40;
    tasks[1].next_run_ms = 10;
    tasks[2].next_run_ms = 30;
    tasks[3].next_run_ms = 20;
    tsched_heap_init(&heap);
    CHECK(tsched_heap_push(&heap, &tasks[0]) == 0);
    CHECK(tsched_heap_push(&heap, &tasks[1]) == 0);
    CHECK(tsched_heap_push(&heap, &tasks[2]) == 0);
    CHECK(tsched_heap_push(&heap, &tasks[3]) == 0);
    CHECK(tsched_heap_pop(&heap)->next_run_ms == 10);
    CHECK(tsched_heap_pop(&heap)->next_run_ms == 20);
    CHECK(tsched_heap_pop(&heap)->next_run_ms == 30);
    CHECK(tsched_heap_pop(&heap)->next_run_ms == 40);
    CHECK(tsched_heap_pop(&heap) == 0);
    return 0;
}
