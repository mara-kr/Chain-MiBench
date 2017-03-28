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
#define MAXARRAY 4 // TODO change to 128
#define STACK_SIZE 4 // 2*ceil(log2(MAXARRAY)), upto next power 2


typedef struct my3DVertexStruct {
    //int x, y, z;
    unsigned distance;
} vertex_t;

typedef struct stack_val {
    unsigned lo, hi;
} stack_val_t;

static int compare(const unsigned elem1, const unsigned elem2) {
    unsigned dist1, dist2;

    dist1 = elem1;
    dist2 = elem2;

    return (dist1 > dist2) ? 1 : ((dist1 == dist2) ? 0 : -1);
}

TASK(1, pre_init)
TASK(2, task_init)
TASK(3, task_sort)
TASK(4, task_end)
TASK(5, bench_fail)
TASK(6, bench_success)

// The stack contains subarrays to be processed (i.e. a call stack)
struct sort_params {
    CHAN_FIELD_ARRAY(unsigned, vals, MAXARRAY);
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
    CHAN_FIELD(unsigned, i);
    CHAN_FIELD_ARRAY(unsigned, vals, MAXARRAY);
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

// Taken from algolist.net/Algorithms/Sorting/Quicksort
static unsigned partition(unsigned low_idx, unsigned hi_idx) {
    unsigned vals[MAXARRAY];
    for (unsigned k = low_idx; k <= hi_idx; k++) {
        vals[k] = *CHAN_IN2(unsigned, vals[k], SELF_IN_CH(task_sort),
                CH(task_init, task_sort));
    }
    unsigned i = low_idx, j = hi_idx;
    unsigned tmp;
    unsigned pivot = vals[(i + j) / 2];

    while (i <= j) {
        while (compare(vals[i], pivot) < 0) { i++;}
        while (compare(vals[j], pivot) > 0) { j--;}
        if (i <= j) {
            tmp = vals[i];
            vals[i] = vals[j];
            vals[j] = tmp;
            i++;
            j++;
        }
    }
    for (unsigned k = low_idx; k <= hi_idx; k++) {
        CHAN_OUT1(unsigned, vals[k], vals[k], SELF_OUT_CH(task_sort));
        if (i <= MAXARRAY) {
        //TODO caused lots of printing garbage
            LOG("vals[%u] = %u\r\n", k, vals[k]);
        }
    }
    return i;
}



void init() {
    WISP_init();

    /* Turn on LEDS */
    GPIO(PORT_LED_1, DIR) |= BIT(PIN_LED_1);
    GPIO(PORT_LED_2, DIR) |= BIT(PIN_LED_2);
#if defined(PORT_LED_3)
    GPIO(PORT_LED_3, DIR) |= BIT(PIN_LED_3);
#endif
    // Turn on LED1
    GPIO(PORT_LED_1, OUT) |= BIT(PIN_LED_1);

    INIT_CONSOLE();

    __enable_interrupt();

    srand(SEED);
}

void pre_init() {
    task_prologue();
    unsigned i = 0;
    CHAN_OUT1(unsigned, i, i, CH(pre_init, task_init));
    TRANSITION_TO(task_init);
}

// Assemble array, transition to task_sort
void task_init() {
    task_prologue();
    LOG("\r\ninit\r\n");

    unsigned i = *CHAN_IN2(unsigned, i, SELF_IN_CH(task_init),
            CH(pre_init, task_init));

    unsigned val;
    for ( ; i < MAXARRAY; i++) {
        val = (unsigned) rand();
        CHAN_OUT1(unsigned, i, i, SELF_OUT_CH(task_init));
        CHAN_OUT1(unsigned, vals[i], val,
                CH(task_init, task_sort));
        LOG("%u:%u\r\n", i, val);
    }

    // Set stack idx to 0xFFFF - need a default value
    unsigned stack_i = 0;
    stack_val_t stack_val;
    stack_val.lo = 0;
    stack_val.hi = MAXARRAY - 1;
    CHAN_OUT1(unsigned, stack_idx, stack_i,
            CH(task_init, task_sort));
    CHAN_OUT1(stack_val_t, stack[0], stack_val, CH(task_init, task_sort));
    TRANSITION_TO(task_sort);
}

void task_sort() {
    task_prologue();
    stack_val_t stack_val, new_val;
    unsigned stack_i, i;
    for (i = 0; i < MAXARRAY; i++) {
        LOG("sort: vals[%u] = %u\r\n", i, *CHAN_IN2(unsigned, vals[i], SELF_IN_CH(task_sort),
                CH(task_init, task_sort)));
    }


    stack_i = *CHAN_IN2(unsigned, stack_idx,
            CH(task_init, task_sort), SELF_IN_CH(task_sort));
    stack_val = *CHAN_IN2(stack_val_t, stack[stack_i],
        CH(task_init, task_sort), SELF_IN_CH(task_sort));
    // NOTE: Chain array symantics are pretty bad
    LOG("sort: stacki=%u, lo = %u, hi = %u\r\n",
            stack_i, stack_val.lo, stack_val.hi);
    if (stack_i < ((unsigned) -1) && (stack_val.lo < stack_val.hi)) {
            i = partition(stack_val.lo, stack_val.hi);
            LOG("i = %u\r\n", i);
            if (stack_val.lo < i - 1) {
                LOG("Adding to stack lo\r\n");
                new_val.lo = stack_val.lo;
                new_val.hi = i - 1;
                CHAN_OUT1(stack_val_t, stack[stack_i], new_val,
                        SELF_OUT_CH(task_sort));
            }
            if (i < stack_val.hi) {
                LOG("Adding to stack hi\r\n");
                if (stack_val.lo < i -1) {
                    stack_i++;
                    CHAN_OUT1(unsigned, stack_idx, stack_i,
                            SELF_OUT_CH(task_sort));
                }
                new_val.lo = i;
                new_val.hi = stack_val.hi;
                CHAN_OUT1(stack_val_t, stack[stack_i], new_val,
                        SELF_OUT_CH(task_sort));
            }
            TRANSITION_TO(task_sort);
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
    unsigned *vals = *CHAN_IN2(unsigned *, vals, SELF_IN_CH(task_end),
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
