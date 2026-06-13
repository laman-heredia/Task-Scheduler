#include "tsched/tsched.h"

static void swap(struct tsched_task **a, struct tsched_task **b)
{
    struct tsched_task *tmp = *a;
    *a = *b;
    *b = tmp;
}

void tsched_heap_init(struct tsched_heap *heap)
{
    heap->count = 0;
}

int tsched_heap_push(struct tsched_heap *heap, struct tsched_task *task)
{
    size_t index;
    if (heap->count >= TSCHED_MAX_TASKS || task->in_heap)
        return -1;
    index = heap->count++;
    heap->items[index] = task;
    task->in_heap = 1;
    /* 新节点自底向上调整，直至父节点的触发时间不晚于当前节点。 */
    while (index > 0) {
        size_t parent = (index - 1U) / 2U;
        if (heap->items[parent]->next_run_ms <= task->next_run_ms)
            break;
        swap(&heap->items[parent], &heap->items[index]);
        index = parent;
    }
    return 0;
}

struct tsched_task *tsched_heap_peek(const struct tsched_heap *heap)
{
    return heap->count ? heap->items[0] : NULL;
}

struct tsched_task *tsched_heap_pop(struct tsched_heap *heap)
{
    struct tsched_task *result;
    size_t index = 0;
    if (!heap->count)
        return NULL;
    result = heap->items[0];
    result->in_heap = 0;
    heap->items[0] = heap->items[--heap->count];
    /* 用末尾节点替换根节点后向下调整，恢复最小堆性质。 */
    while (index < heap->count) {
        size_t left = index * 2U + 1U;
        size_t right = left + 1U;
        size_t smallest = index;
        if (left < heap->count &&
            heap->items[left]->next_run_ms < heap->items[smallest]->next_run_ms)
            smallest = left;
        if (right < heap->count &&
            heap->items[right]->next_run_ms < heap->items[smallest]->next_run_ms)
            smallest = right;
        if (smallest == index)
            break;
        swap(&heap->items[index], &heap->items[smallest]);
        index = smallest;
    }
    return result;
}

int tsched_heap_remove(struct tsched_heap *heap, struct tsched_task *task)
{
    size_t index;
    if (!task->in_heap)
        return 0;
    for (index = 0; index < heap->count; ++index)
        if (heap->items[index] == task)
            break;
    if (index == heap->count) {
        task->in_heap = 0;
        return -1;
    }
    task->in_heap = 0;
    heap->items[index] = heap->items[--heap->count];
    if (index < heap->count) {
        size_t parent = index ? (index - 1U) / 2U : 0;
        if (index && heap->items[index]->next_run_ms <
                     heap->items[parent]->next_run_ms) {
            while (index > 0) {
                parent = (index - 1U) / 2U;
                if (heap->items[parent]->next_run_ms <=
                    heap->items[index]->next_run_ms)
                    break;
                swap(&heap->items[parent], &heap->items[index]);
                index = parent;
            }
        } else {
            while (index < heap->count) {
                size_t left = index * 2U + 1U;
                size_t right = left + 1U;
                size_t smallest = index;
                if (left < heap->count &&
                    heap->items[left]->next_run_ms <
                    heap->items[smallest]->next_run_ms)
                    smallest = left;
                if (right < heap->count &&
                    heap->items[right]->next_run_ms <
                    heap->items[smallest]->next_run_ms)
                    smallest = right;
                if (smallest == index)
                    break;
                swap(&heap->items[index], &heap->items[smallest]);
                index = smallest;
            }
        }
    }
    return 0;
}
