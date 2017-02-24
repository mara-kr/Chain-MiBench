#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include <libio/log.h>
#include <libchain/chain.h>

#ifdef CONFIG_LIBEDB_PRINTF
#include <libedb.edb.h>
#endif

#define UNLIMIT
#define SEED 2 // No guarentuee of on-board timer
#define MAXARRAY 4096 /* too large causes segfault (orig == 60000)*/
#define STACK_SIZE 32 // 2*ceil(log2(MAXARRAY)), upto next power 2


typedef struct my3DVertexStruct {
    int x, y, z;
    double distance;
} vertex_t;

typedef struct stack_val {
    unsigned lo, hi;
} stack_val_t;

static int compare(const vertex_t elem1, const vertex_t elem2) {
    double dist1, dist2;

    dist1 = elem1.distance;
    dist2 = elem2.distance;

    return (dist1 > dist2) ? 1 : ((dist1 == dist2) ? 0 : -1);
}

// Taken from algolist.net/Algorithms/Sorting/Quicksort
static unsigned partition(vertex_t *a, unsigned low_idx, unsigned hi_idx) {
    unsigned i = low_idx, j = hi_idx;
    vertex_t tmp;
    vertex_t pivot = a[(i + j) / 2];

    while (i <= j) {
        while (compare(a[i], pivot) < 0) { i++;}
        while (compare(a[j], pivot) > 0) { j--;}
        if (i <= j) {
            tmp = a[i];
            a[i] = a[j];
            a[j] = tmp;
        }
    }
    return i;
}

TASK(1, pre_init)
TASK(2, task_init)
TASK(3, task_sort)
TASK(4, task_end)

// The stack contains subarrays to be processed (i.e. a call stack)
struct sort_params {
    CHAN_FIELD_ARRAY(vertex_t, vals, MAXARRAY);
    CHAN_FIELD_ARRAY(stack_val_t, stack, STACK_SIZE);
    CHAN_FIELD(unsigned, stack_idx);
};

#define FIELD_INIT_sort_params {\
    SELF_FIELD_ARRAY_INITIALIZER(MAXARRAY),\
    SELF_FIELD_ARRAY_INITIALIZER(STACK_SIZE),\
    SELF_FIELD_INITIALIZER\
}

struct init_i {
    SELF_CHAN_FIELD(unsigned, i);
};

#define FIELD_INIT_init_i {\
    SELF_FIELD_INITIALIZER\
}

// Add array field
struct end_i {
    SELF_CHAN_FIELD(unsigned, i);
    CHAN_FIELD_ARRAY(vertex_t, vals, MAXARRAY);
};

#define FIELD_INIT_end_i {\
    SELF_FIELD_INITIALIZER\
}

CHANNEL(pre_init, task_init, init_i);
CHANNEL(task_init, task_sort, sort_params);
CHANNEL(task_sort, task_end, end_i);
SELF_CHANNEL(task_sort, sort_params);
SELF_CHANNEL(task_init, init_i);
SELF_CHANNEL(task_end, end_i);


// always run on reboot
void init() {
    srand(SEED);
}

void pre_init() {
    unsigned i = 0;
    CHAN_OUT1(unsigned, i, i, CH(pre_init, task_init));
    TRANSITION_TO(task_init);
}

// Assemble array, transition to task_sort
void task_init() {
    task_prologue();
    LOG("init\r\n");
    unsigned i;
    i = *CHAN_IN2(unsigned, i, SELF_IN_CH(task_init),
            CH(pre_init, task_init));

    vertex_t vals[MAXARRAY];
    for ( ; i < MAXARRAY; i++) {
        vals[i].distance = (double) rand();
        vals[i].x = rand();
        vals[i].y = rand();
        vals[i].z = rand();
        CHAN_OUT1(unsigned, i, i, SELF_OUT_CH(task_init));
    }

    // Set stack idx to 0xFFFF - need a default value
    unsigned stack_i = (unsigned) -1;
    CHAN_OUT1(unsigned, stack_idx, stack_i,
            CH(task_init, task_sort));
    CHAN_OUT1(vertex_t *, vals, vals,
            CH(task_init, task_sort));
    TRANSITION_TO(task_sort);
}

void task_sort() {
    task_prologue();
    vertex_t *vals;
    stack_val_t *stack;
    stack_val_t stack_val;
    unsigned stack_i, i;

    stack_i = *CHAN_IN2(unsigned, stack_idx,
            CH(task_init, task_sort), SELF_IN_CH(task_sort));
    stack = *CHAN_IN2(stack_val_t *, stack,
        CH(task_init, task_sort), SELF_IN_CH(task_sort));
    vals = *CHAN_IN2(vertex_t *, vals,
        CH(task_init, task_sort), SELF_IN_CH(task_sort));
    if (stack_i < ((unsigned) -1)) {
        stack_val = stack[stack_i];
        if (stack_val.lo < stack_val.hi) {
            i = partition(vals, stack_val.lo, stack_val.hi);
            stack[stack_i] = (stack_val_t) {.lo = stack_val.lo, .hi = i-1};
            CHAN_OUT1(stack_val_t, stack[stack_i], stack[stack_i],
                    SELF_OUT_CH(task_sort));
            stack_i++;
            stack[stack_i] = (stack_val_t) {.lo = i, .hi = stack_val.hi};
            CHAN_OUT1(stack_val_t, stack[stack_i], stack[stack_i],
                    SELF_OUT_CH(task_sort));

            CHAN_OUT1(vertex_t *, vals, vals,
                    SELF_OUT_CH(task_sort));
            CHAN_OUT1(unsigned, stack_idx, stack_i,
                    SELF_OUT_CH(task_sort));
            TRANSITION_TO(task_sort);
        }
    } else {
        unsigned int end_i = 0;
        CHAN_OUT1(unsigned, i, i, CH(task_sort, task_end));
        TRANSITION_TO(task_end);
    }
}

void task_end() {
    task_prologue();

    unsigned i = *CHAN_IN2(unsigned, i, SELF_IN_CH(task_end),
            CH(task_sort, task_end));
    vertex_t *vals = *CHAN_IN2(vertex_t *, vals, SELF_IN_CH(task_end),
            CH(task_sort, task_end));

    for ( ; i < MAXARRAY-1; i++) {
        CHAN_OUT1(unsigned, i, i, SELF_OUT_CH(task_end));
        if (compare(vals[i+1],vals[i]) < 0) {
            // Some failure condition
        }
    }
}