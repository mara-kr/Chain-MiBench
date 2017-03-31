#include <msp430.h>
#include <stdlib.h>

#include <libio/log.h>
#include <libchain/chain.h>
#include <libwispbase/wisp-base.h>

#include "pin_assign.h"


#define WAIT_TICK_DURATION_ITERS 300000
#define NUM_VALS 8
#define SEED 4

uint8_t usrBank[USRBANK_SIZE];

volatile unsigned work_x;
static void burn(uint32_t iters) {
    uint32_t iter = iters;
    while (iter--)
        work_x++;
}


struct bit_vals {
    CHAN_FIELD_ARRAY(unsigned, vals, NUM_VALS);
    CHAN_FIELD_ARRAY(unsigned, results, NUM_VALS);
    CHAN_FIELD(unsigned, index);
};

struct init_i {
    SELF_CHAN_FIELD(unsigned, index);
};

#define FIELD_INIT_init_i {\
    SELF_FIELD_INITIALIZER\
}

CHANNEL(pre_init, task_init, init_i);
CHANNEL(task_init, task_bitcount, bit_vals);
SELF_CHANNEL(task_bitcount, init_i);
SELF_CHANNEL(task_init, init_i);

TASK(1, pre_init)
TASK(2, task_init)
TASK(3, task_bitcount)
TASK(4, task_end)


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

    LOG("main.c booted\r\n");
    srand(SEED);
}

void pre_init() {
    task_prologue();
    LOG("pre_init");
    unsigned i = 0;
    CHAN_OUT1(unsigned, index, i, CH(pre_init, task_init));
    TRANSITION_TO(task_init);
}

void task_init() {
    task_prologue();
    LOG("init\r\n");

    unsigned i;
    i = *CHAN_IN2(unsigned, index, SELF_IN_CH(task_init),
            CH(pre_init, task_init));

    unsigned vals[NUM_VALS];
    unsigned results[NUM_VALS];
    for ( ; i < NUM_VALS; i++) {
        vals[i] = (unsigned) rand();
        results[i] = 0;
        CHAN_OUT1(unsigned, index, i, SELF_OUT_CH(task_init));
        CHAN_OUT1(unsigned, vals[i], vals[i], CH(task_init, task_bitcount));
        CHAN_OUT1(unsigned, results[i], vals[i], CH(task_init, task_bitcount));
        LOG("START %x:%x, %x\r\n", i, vals[i], results[i]);
    }
    i = 0;
    CHAN_OUT1(unsigned, index, i, CH(task_init, task_bitcount));
    TRANSITION_TO(task_bitcount);
}

void task_bitcount() {
    task_prologue();
    unsigned i, val, count;
    unsigned *vals, *results;
    i = *CHAN_IN2(unsigned, index, CH(task_init, task_bitcount),
            SELF_IN_CH(task_bitcount));
    results = *CHAN_IN1(unsigned *, results, CH(task_init, task_bitcount));
    for ( ; i < NUM_VALS; i++) {
        count = 0;
        val = *CHAN_IN1(unsigned, vals[i], CH(task_init, task_bitcount));
        LOG("val %u=%x\r\n", i, val);
        // Do the counting
        if (val) {
            do {
                count++;
                val = val & (val - 1);
            } while (val);
        }

        results[i] = count;
        CHAN_OUT1(unsigned, index, i, SELF_OUT_CH(task_bitcount));
        LOG("END %x: %x\r\n", i, results[i]);
    }
    TRANSITION_TO(task_end);
}

void task_end() {
    //LOG("End");
    GPIO(PORT_LED_2, OUT) |= BIT(PIN_LED_2);
    burn(WAIT_TICK_DURATION_ITERS);
    GPIO(PORT_LED_2, OUT) &= ~BIT(PIN_LED_2);
    burn(WAIT_TICK_DURATION_ITERS);
    TRANSITION_TO(task_end);
}

ENTRY_TASK(pre_init)
INIT_FUNC(init)
