#include <msp430.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include <libwispbase/wisp-base.h>
#include <libio/log.h>
#include <libchain/chain.h>

#ifdef CONFIG_LIBEDB_PRINTF
#include <libedb/edb.h>
#endif

#include "pin_assign.h"

#define UNLIMIT

#define WAIT_TICK_DURATION_ITERS 300000

#define SEED 2 // No guarentuee of on-board timer
#define MAXARRAY 256 // Any larger won't fit on msp430
#define STACK_SIZE 32 // 2*ceil(log2(MAXARRAY)), upto next power 2


typedef struct my3DVertexStruct {
    //int x, y, z;
    double distance;
} vertex_t;

typedef struct stack_val {
    unsigned lo, hi;
} stack_val_t;

static int compare(const double elem1, const double elem2) {
    double dist1, dist2;

    dist1 = elem1;
    dist2 = elem2;

    return (dist1 > dist2) ? 1 : ((dist1 == dist2) ? 0 : -1);
}

// Taken from algolist.net/Algorithms/Sorting/Quicksort
static unsigned partition(double *a, unsigned low_idx, unsigned hi_idx) {
    unsigned i = low_idx, j = hi_idx;
    double tmp;
    double pivot = a[(i + j) / 2];

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
TASK(5, bench_fail)
TASK(6, bench_success)

// The stack contains subarrays to be processed (i.e. a call stack)
struct sort_params {
    CHAN_FIELD_ARRAY(double, vals, MAXARRAY);
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
struct end_vals {
    SELF_CHAN_FIELD(unsigned, i);
    //CHAN_FIELD_ARRAY(double, vals, MAXARRAY);
    SELF_CHAN_FIELD_ARRAY(double, vals, MAXARRAY);
};

#define FIELD_INIT_end_vals {\
    SELF_FIELD_INITIALIZER,\
    SELF_FIELD_ARRAY_INITIALIZER(MAXARRAY)\
}

CHANNEL(pre_init, task_init, init_i);
CHANNEL(task_init, task_sort, sort_params);
CHANNEL(task_sort, task_end, end_vals);
SELF_CHANNEL(task_sort, sort_params);
SELF_CHANNEL(task_init, init_i);
SELF_CHANNEL(task_end, end_vals);

volatile unsigned work_x;

static void burn(uint32_t iters) {
    uint32_t iter = iters;
    while (iter--)
        work_x++;
}

// always run on reboot
void init() {
    srand(SEED);
    /* Turn on LEDS */
    GPIO(PORT_LED_1, DIR) |= BIT(PIN_LED_1);
    GPIO(PORT_LED_2, DIR) |= BIT(PIN_LED_2);
#if defined(PORT_LED_3)
    GPIO(PORT_LED_3, DIR) |= BIT(PIN_LED_3);
#endif
    // Turn on LED1 until we (maybe) fail
    GPIO(PORT_LED_1, OUT) |= BIT(PIN_LED_1);

    INIT_CONSOLE();
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
    GPIO(PORT_LED_1, OUT) |= BIT(PIN_LED_1);

    unsigned i;
    i = *CHAN_IN2(unsigned, i, SELF_IN_CH(task_init),
            CH(pre_init, task_init));

    double vals[MAXARRAY];
    for ( ; i < MAXARRAY; i++) {
        vals[i] = (double) rand();
        CHAN_OUT1(unsigned, i, i, SELF_OUT_CH(task_init));
    }

    // Set stack idx to 0xFFFF - need a default value
    unsigned stack_i = (unsigned) -1;
    CHAN_OUT1(unsigned, stack_idx, stack_i,
            CH(task_init, task_sort));
    CHAN_OUT1(double *, vals, vals,
            CH(task_init, task_sort));
    TRANSITION_TO(task_sort);
}

void task_sort() {
    task_prologue();
    double *vals;
    stack_val_t *stack;
    stack_val_t stack_val;
    unsigned stack_i, i;

    stack_i = *CHAN_IN2(unsigned, stack_idx,
            CH(task_init, task_sort), SELF_IN_CH(task_sort));
    stack = *CHAN_IN2(stack_val_t *, stack,
        CH(task_init, task_sort), SELF_IN_CH(task_sort));
    vals = *CHAN_IN2(double *, vals,
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

            CHAN_OUT1(double *, vals, vals,
                    SELF_OUT_CH(task_sort));
            CHAN_OUT1(unsigned, stack_idx, stack_i,
                    SELF_OUT_CH(task_sort));
            TRANSITION_TO(task_sort);
        }
    } else {
        unsigned int end_i = 0;
        CHAN_OUT1(unsigned, i, end_i, CH(task_sort, task_end));
        TRANSITION_TO(task_end);
    }
}

void task_end() {
    task_prologue();

    unsigned i = *CHAN_IN2(unsigned, i, SELF_IN_CH(task_end),
            CH(task_sort, task_end));
    double *vals = *CHAN_IN2(double *, vals, SELF_IN_CH(task_end),
            CH(task_sort, task_end));

    for ( ; i < MAXARRAY-1; i++) {
        CHAN_OUT1(unsigned, i, i, SELF_OUT_CH(task_end));
        if (compare(vals[i+1],vals[i]) < 0) {
            TRANSITION_TO(bench_fail);
        }
    }
    TRANSITION_TO(bench_success);
}

// Blink LED1 on failure
void bench_fail() {
    GPIO(PORT_LED_1, OUT) |= BIT(PIN_LED_1);
    burn(WAIT_TICK_DURATION_ITERS);
    GPIO(PORT_LED_1, OUT) &= ~BIT(PIN_LED_1);
    burn(WAIT_TICK_DURATION_ITERS);
}

// Blink LED2 on success
void bench_success() {
    GPIO(PORT_LED_2, OUT) |= BIT(PIN_LED_2);
    burn(WAIT_TICK_DURATION_ITERS);
    GPIO(PORT_LED_2, OUT) &= ~BIT(PIN_LED_2);
    burn(WAIT_TICK_DURATION_ITERS);
}

ENTRY_TASK(pre_init)
INIT_FUNC(init)
