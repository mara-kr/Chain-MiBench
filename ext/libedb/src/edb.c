#include <msp430.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <libwispbase/wisp-base.h>
#include <libmsp/periph.h>
#include <libmsp/mem.h>

#ifdef __clang__
#include <msp-builtins.h>
#endif

#include "edb.h"
#include "pin_assign.h"
#include "target_comm.h"

#define DEBUG_RETURN                0x0001 // signals debug main loop to stop
#define DEBUG_REQUESTED_BY_TARGET   0x0002 // the target requested to enter debug mode

#define DEBUG_MODE_REQUEST_WAIT_STATE_BITS      LPM0_bits
#define DEBUG_MODE_EXIT_WAIT_STATE_BITS         LPM0_bits

#define TX_BUF_SIZE 64

static uint8_t tx_buf[TX_BUF_SIZE];

static app_output_cb_t *app_output_cb = NULL;

typedef enum {
    STATE_IDLE,
    STATE_DEBUG,
    STATE_SUSPENDED,
} state_t;

typedef struct {
	uint16_t CSCTL0;
	uint16_t CSCTL1;
	uint16_t CSCTL2;
	uint16_t CSCTL3;
	uint16_t CSCTL4;
	uint16_t CSCTL5;
	uint16_t CSCTL6;
} clkInfo_t;

typedef enum {
	MSG_STATE_IDENTIFIER,	//!< UART identifier byte
	MSG_STATE_DESCRIPTOR,	//!< UART descriptor byte
	MSG_STATE_DATALEN,		//!< data length byte
	MSG_STATE_PADDING,      //!< padding in header for alignment
	MSG_STATE_DATA,		    //!< UART data
} msgState_t;

typedef struct {
    uint8_t descriptor;
    uint8_t len;
    uint8_t *data;
} cmd_t;

typedef struct {
    interrupt_type_t type;
    uint16_t id;
    uint8_t features;
} interrupt_context_t;

static state_t state = STATE_IDLE;


// This is a hack. We use this pin for a different purpose, not related to
// libedb.
//static bool sig_active = false;

static uint16_t debug_flags = 0;
static interrupt_context_t interrupt_context;

volatile __nv uint16_t _libedb_internal_breakpoints = 0x00;

static uint16_t pc; // program counter at time of interrupt (retrieved from stack)

// expecting 2-byte messages from the debugger (identifier byte + descriptor byte)
static uint8_t uartRxBuf[CONFIG_DEBUG_UART_BUF_LEN];

static uint8_t cmd_data_buf[WISP_CMD_MAX_LEN];

static void set_state(state_t new_state)
{
#ifdef CONFIG_STATE_PINS
    uint8_t port_value;
#endif

    state = new_state;

#ifdef CONFIG_STATE_PINS
    // Encode state onto two indicator pins
    port_value = GPIO(PORT_STATE, OUT);
    port_value &= ~(BIT(PIN_STATE_0) | BIT(PIN_STATE_1)); // clear
    port_value |= (new_state & 0x1 ? BIT(PIN_STATE_0) : 0) |
                  (new_state & 0x2 ? BIT(PIN_STATE_1) : 0);
    GPIO(PORT_STATE, OUT) = port_value;
#endif
}

static void signal_debugger()
{
    // pulse the signal line

    // target signal line starts in high imedence state
    GPIO(PORT_SIG, OUT) |= BIT(PIN_SIG);        // output high
    GPIO(PORT_SIG, DIR) |= BIT(PIN_SIG);        // output enable
    GPIO(PORT_SIG, OUT) &= ~BIT(PIN_SIG);    // output low
    GPIO(PORT_SIG, DIR) &= ~BIT(PIN_SIG);    // back to high impedence state
    GPIO(PORT_SIG, IFG) &= ~BIT(PIN_SIG); // clear interrupt flag (might have been set by the above)
}

#ifdef CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE
static void signal_debugger_with_data(uint8_t data)
{
    unsigned i;
    uint8_t bit;
    uint8_t port_bits[SIG_SERIAL_NUM_BITS];

    // Precompute all port values in order to keep the bit duration constant,
    // i.e. so that it does not vary with the bit index and bit value.
    for (i = 0; i < SIG_SERIAL_NUM_BITS; ++i) {
        bit = (data >> i) & 0x1;
        port_bits[i] = bit << PIN_SIG;
    }

    __disable_interrupt();

    // target signal line starts in high imedence state

    // starting pulse
    GPIO(PORT_SIG, OUT) |= BIT(PIN_SIG);        // output high
    GPIO(PORT_SIG, DIR) |= BIT(PIN_SIG);        // output enable
    GPIO(PORT_SIG, OUT) &= ~BIT(PIN_SIG);    // output low

    // Need constant and short time between bits, so no loops or conditionals
#define PULSE_BIT(idx) \
    __delay_cycles(SIG_SERIAL_BIT_DURATION_ON_TARGET); \
    GPIO(PORT_SIG, OUT) |= port_bits[idx]; \
    GPIO(PORT_SIG, OUT) &= ~BIT(PIN_SIG); \

#if SIG_SERIAL_NUM_BITS > 3
    PULSE_BIT(3);
#endif
#if SIG_SERIAL_NUM_BITS > 2
    PULSE_BIT(2);
#endif
#if SIG_SERIAL_NUM_BITS > 1
    PULSE_BIT(1);
#endif
#if SIG_SERIAL_NUM_BITS > 0
    PULSE_BIT(0);
#endif

    // terminating pulse: must happen after the interval for the last bit elapses
    __delay_cycles(SIG_SERIAL_BIT_DURATION_ON_TARGET); // ignore the few compute instructions
    GPIO(PORT_SIG, OUT) |= BIT(PIN_SIG);        // output high
    GPIO(PORT_SIG, OUT) &= ~BIT(PIN_SIG);    // output low

    GPIO(PORT_SIG, DIR) &= ~BIT(PIN_SIG);    // back to high impedence state
    GPIO(PORT_SIG, IFG) &= ~BIT(PIN_SIG); // clear interrupt flag (might have been set by the above)

    __enable_interrupt();
}
#endif // CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE


static void unmask_debugger_signal()
{
    //sig_active = true;
    GPIO(PORT_SIG, IES) &= ~BIT(PIN_SIG); // rising edge
    GPIO(PORT_SIG, IFG) &= ~BIT(PIN_SIG); // clear the flag that might have been set by IES write
    GPIO(PORT_SIG, IE) |= BIT(PIN_SIG); // enable interrupt
}

static void mask_debugger_signal()
{
    GPIO(PORT_SIG, IE) &= ~BIT(PIN_SIG); // disable interrupt
    GPIO(PORT_SIG, IFG) &= ~BIT(PIN_SIG);
    //sig_active = false;
}

static void clear_interrupt_context()
{
    interrupt_context.type = INTERRUPT_TYPE_NONE;
    interrupt_context.id = 0;
    interrupt_context.features = 0;
}

static void enter_debug_mode()
{
    __enable_interrupt();

    if (interrupt_context.features & DEBUG_MODE_WITH_UART)
        UART_init();

    set_state(STATE_DEBUG);
}

void exit_debug_mode()
{
    if (interrupt_context.features & DEBUG_MODE_WITH_UART)
        UART_teardown();

    clear_interrupt_context();
}

/**
 * @brief Send a message in response to debugger's request to
 *        enter debug mode on boot.
 *
 * @details We have no choice but to communicate to EDB via UART
 *          because, when EDB requests the target to enter debug
 *          mode on boot, EDB drives the signal line high, so
 *          we can't use that line to communicate.
 */
void send_interrupted_msg()
{
    // Prepare settings to be applied on wakeup from ISR (see below)
    debug_flags |= DEBUG_REQUESTED_BY_TARGET;
    interrupt_context.type = INTERRUPT_TYPE_DEBUGGER_REQ;
    interrupt_context.features = DEBUG_MODE_FULL_FEATURES;

    unmask_debugger_signal();

    UART_init();

    unsigned msg_len = 0;
    tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
    tx_buf[msg_len++] = WISP_RSP_INTERRUPTED;
    tx_buf[msg_len++] = 0;
    tx_buf[msg_len++] = 0; // padding

    UART_send(tx_buf, msg_len);

    // Wait for debugger to turn on power supply and notify us
    __bis_SR_register(DEBUG_MODE_REQUEST_WAIT_STATE_BITS | GIE);

    // On wakeup, we end up in the target<->debugger signal ISR
}

#ifdef CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE
void request_debug_mode(interrupt_type_t int_type, unsigned int_id, unsigned features)
{
    // Disable interrupts before unmasking debugger signal to make sure
    // we are asleep (at end of this function) before ISR runs. Otherwise,
    // the race completely derails the sequence to enter-exit debug mode.
    // Furthermore, to prevent a signal from the debugger arriving while
    // we are trying to request debug mode, disable interrupts at the
    // very beginning of this function.
    __disable_interrupt();

    debug_flags |= DEBUG_REQUESTED_BY_TARGET;
    interrupt_context.type = int_type;
    interrupt_context.id = int_id;
    interrupt_context.features = features;

    mask_debugger_signal();

    switch (state) {
        case STATE_DEBUG: // an assert/breakpoint nested in an energy guard
            signal_debugger_with_data(SIG_CMD_INTERRUPT);
            break;
        default: // hot path (hit an assert/bkpt), we want the debugger to take action asap
            signal_debugger();
    }

    unmask_debugger_signal();

    // go to sleep, enable interrupts, and wait for signal from debugger
    __bis_SR_register(DEBUG_MODE_REQUEST_WAIT_STATE_BITS | GIE);
}

// NOTE: This is a function because putting a call to request_debug_mode
// in the PRINTF() macro doesn't work with Clang, which has a different
// calling convention from GCC. And, we have to compile libedb with GCC,
// because Clang compilation of it doesn't function correctly (probably
// because of delay variations, etc.).
void request_non_interactive_debug_mode()
{
    request_debug_mode(INTERRUPT_TYPE_ENERGY_GUARD, 0, DEBUG_MODE_WITH_UART);
}

// Same comment applies as above
void request_energy_guard_debug_mode()
{
    request_debug_mode(INTERRUPT_TYPE_ENERGY_GUARD, 0, DEBUG_MODE_WITH_UART);
}
#endif // CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE

#ifdef CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE // resume_application used only for energy guards
void resume_application()
{
    exit_debug_mode();

    set_state(STATE_SUSPENDED); // sleep and wait for debugger to restore energy

    mask_debugger_signal();

    // Disable interrupts before unmasking debugger signal to make sure
    // we are asleep (at end of this function) before ISR runs. Otherwise,
    // the race completely derails the sequence to enter-exit debug mode.
    // Furthermore, to prevent a signal from the debugger arriving while
    // we are trying to request debug mode, disable interrupts at the
    // very beginning of this function.
    __disable_interrupt();

    // debugger is in DEBUG state, so our signal needs to contain
    // the information about whether we are exiting the debug mode
    // (as we are here) or whether we are requesting a nested debug
    // mode due to an assert/bkpt.
    signal_debugger_with_data(SIG_CMD_EXIT); // tell debugger we have shutdown UART

    unmask_debugger_signal();

    // go to sleep, enable interrupts, and wait for signal from debugger
    __bis_SR_register(DEBUG_MODE_REQUEST_WAIT_STATE_BITS | GIE);
}
#endif

uintptr_t mem_addr_from_bytes(uint8_t *buf)
{
    return (uintptr_t)
        (((uint32_t)buf[3] << 24) |
        ((uint32_t)buf[2] << 16) |
        ((uint32_t)buf[1] << 8) |
        ((uint32_t)buf[0] << 0));
}

static void execute_cmd(cmd_t *cmd)
{
    uint8_t msg_len;
    uint8_t *address;
    uint8_t offset;
    uint8_t len;
    uint8_t i;
    switch (cmd->descriptor)
    {
        case WISP_CMD_GET_PC:
        {
            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_ADDRESS;
            tx_buf[msg_len++] = sizeof(uint32_t);
            tx_buf[msg_len++] = 0; // padding
            tx_buf[msg_len++] = ((uintptr_t)pc >> 0) & 0xff;
            tx_buf[msg_len++] = ((uintptr_t)pc >> 8) & 0xff;
            tx_buf[msg_len++] = 0; // TODO: 20-bit ptr
            tx_buf[msg_len++] = 0;

            UART_send(tx_buf, msg_len);
            break;
        }
        case WISP_CMD_READ_MEM:
        {
            // TODO: assert(msg->len == 4)
            uint8_t max_len = TX_BUF_SIZE - (UART_MSG_HEADER_SIZE + sizeof(uint32_t)); /* addr */

            offset = 0;
            address = (uint8_t *)mem_addr_from_bytes(&cmd->data[offset]); // TODO: 20-bit ptr
            offset += sizeof(uint32_t);
            len = cmd->data[offset];
            offset += sizeof(uint8_t);

            if (len > max_len)
                len = max_len;

            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_MEMORY;
            tx_buf[msg_len++] = sizeof(uint32_t) + len;
            tx_buf[msg_len++] = 0; // padding
            tx_buf[msg_len++] = ((uintptr_t)address >> 0) & 0xff;
            tx_buf[msg_len++] = ((uintptr_t)address >> 8) & 0xff;
            tx_buf[msg_len++] = 0; // TODO: 20-bit ptr
            tx_buf[msg_len++] = 0;

            for (i = 0; i < len; ++i)
                tx_buf[msg_len++] = *address++;

            UART_send(tx_buf, msg_len);
            break;
        }
        case WISP_CMD_WRITE_MEM:
        {
            // TODO: assert(msg->len >= 5)

            offset = 0;
            address = (uint8_t *)mem_addr_from_bytes(&cmd->data[offset]); // TODO: 20-bit ptr
            offset += sizeof(uint32_t);
            len = cmd->data[offset];
            offset += sizeof(uint8_t);
            uint8_t *value = &cmd->data[offset];

            for (i =  0; i < len; ++i) {
                *address = *value++;
                address++;
            }

            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_MEMORY;
            tx_buf[msg_len++] = sizeof(uint32_t) + sizeof(uint8_t);
            tx_buf[msg_len++] = 0; // padding
            tx_buf[msg_len++] = ((uintptr_t)address >> 0) & 0xff;
            tx_buf[msg_len++] = ((uintptr_t)address >> 8) & 0xff;
            tx_buf[msg_len++] = 0; // TODO: 20-bit ptr
            tx_buf[msg_len++] = 0;
            tx_buf[msg_len++] = *address;

            UART_send(tx_buf, msg_len);
            break;
        }
        case WISP_CMD_BREAKPOINT:
        {
            uint8_t index = cmd->data[0];
            bool enable = cmd->data[1];

            if (enable)
                _libedb_internal_breakpoints |= 1 << index;
            else
                _libedb_internal_breakpoints &= ~(1 << index);

            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_BREAKPOINT;
            tx_buf[msg_len++] = 0; // length
            tx_buf[msg_len++] = 0; // padding

            UART_send(tx_buf, msg_len);
            break;
        }
        case WISP_CMD_EXIT_ACTIVE_DEBUG:
            exit_debug_mode();
            debug_flags |= DEBUG_RETURN; // return from debug_main
            break;
        
        case WISP_CMD_GET_INTERRUPT_CONTEXT:
            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_INTERRUPT_CONTEXT;
            tx_buf[msg_len++] = 3 * sizeof(uint8_t);
            tx_buf[msg_len++] = 0; // padding
            tx_buf[msg_len++] = interrupt_context.type;
            tx_buf[msg_len++] = interrupt_context.id;
            tx_buf[msg_len++] = interrupt_context.id >> 8;

            UART_send(tx_buf, msg_len);
            break;

        case WISP_CMD_SERIAL_ECHO: {
            uint8_t echo_value = cmd->data[0];

            mask_debugger_signal();
#ifdef CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE
            signal_debugger_with_data(echo_value);
#else
            (void)echo_value;
            signal_debugger();
#endif
            unmask_debugger_signal();

            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_SERIAL_ECHO;
            tx_buf[msg_len++] = 0; // length
            tx_buf[msg_len++] = 0; // padding

            UART_send(tx_buf, msg_len);
            break;
        }

        case WISP_CMD_GET_APP_OUTPUT: {
            unsigned len = TX_BUF_SIZE - UART_MSG_HEADER_SIZE;
            unsigned len_field_offset;

            msg_len = 0;
            tx_buf[msg_len++] = UART_IDENTIFIER_WISP;
            tx_buf[msg_len++] = WISP_RSP_APP_OUTPUT;
            len_field_offset = msg_len;
            tx_buf[msg_len++] = 0; // length: to be filled out shortly
            tx_buf[msg_len++] = 0; // padding

            if (app_output_cb != NULL)
                app_output_cb(tx_buf + msg_len, &len);
            else
                len = 0;

            msg_len += len;
            tx_buf[len_field_offset] = len;

            UART_send(tx_buf, msg_len);
            break;
        }

        default: // invalid cmd
            break;
    }
}

/**
 * @brief	Parse and handle cmds that come from the debugger over UART
 * @return  Whether a command was parsed and is ready for execution
 */
static bool parse_cmd(cmd_t *cmd, uint8_t *msg, uint8_t len)
{
    static msgState_t msg_state = MSG_STATE_IDENTIFIER;
    static uint8_t data_len = 0;

    uint8_t i;
    for(i = 0; i < len; i++) {
        switch(msg_state)
        {
            case MSG_STATE_IDENTIFIER:
                {
                    uint8_t identifier = msg[i];
                    if(identifier == UART_IDENTIFIER_WISP) {
                        // good identifier byte
                        msg_state = MSG_STATE_DESCRIPTOR;
                    }

                    // else we had a bad identifier byte, so don't change the state
                    break;
                }

            case MSG_STATE_DESCRIPTOR:
                data_len = 0;
                cmd->descriptor = msg[i];
                cmd->len = 0;
                msg_state = MSG_STATE_DATALEN;
                break;

            case MSG_STATE_DATALEN:
                data_len = msg[i]; // decremented as data bytes are parsed
                msg_state = MSG_STATE_PADDING;
                break;

            case MSG_STATE_PADDING:
                if (data_len) {
                    msg_state = MSG_STATE_DATA;
                } else { // done
                    msg_state = MSG_STATE_IDENTIFIER;
                    return true;
                }
                break;

            case MSG_STATE_DATA:
                if (data_len)
                    cmd->data[cmd->len++] = msg[i];
                if (--data_len == 0) {
                    msg_state = MSG_STATE_IDENTIFIER;
                    return true;
                }
                break;

            default:
                break;
        }
    }

    return false;
}

/**
 * @brief    Debug mode main loop.  This executes when the WISP enters debug mode,
 *             and should allow debugging functionality.
 */
static void debug_main()
{
    cmd_t cmd = { .data = cmd_data_buf };
#ifdef LED_IN_DEBUG_STATE
    GPIO(PORT_DEBUG_MODE_LED, OUT) |= BIT(PIN_DEBUG_MODE_LED);
#endif

// NOTE: On these two boards all message sizes should be multiples of header
// size. If we try to read byte by byte, we miss the bytes. Not sure why it
// works fine on the EDB board.
#if defined(BOARD_SPRITE_APP_SOCKET_RHA) || defined(BOARD_SPRITE_APP)
#define CHUNK_BYTES UART_MSG_HEADER_SIZE
#else
#define CHUNK_BYTES 1
#endif

    while(1) {

        // block until we receive a message
        UART_receive(uartRxBuf, CHUNK_BYTES);
        if (parse_cmd(&cmd, uartRxBuf, CHUNK_BYTES)) {
            execute_cmd(&cmd);
        }

        if(debug_flags & DEBUG_RETURN) {
            debug_flags &= ~DEBUG_RETURN;
            break;
        }
    }

#ifdef LED_IN_DEBUG_STATE
    GPIO(PORT_DEBUG_MODE_LED, OUT) &= ~BIT(PIN_DEBUG_MODE_LED);
#endif
}

static inline void handle_debugger_signal()
{
    switch (state) {
        case STATE_IDLE: // debugger requested debug mode on boot, we responded, and debugger responded
        case STATE_DEBUG: // debugger requested to enter a *nested* debug mode

            // If entering debug mode on debugger's initiative (i.e. when we
            // didn't request it), then need to set the features.
            if (interrupt_context.type == INTERRUPT_TYPE_NONE) {
                interrupt_context.type = INTERRUPT_TYPE_DEBUGGER_REQ;
                interrupt_context.features = DEBUG_MODE_FULL_FEATURES;
            }

            enter_debug_mode();

#ifdef CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE
            // If target initiated the debug mode request then, send the
            // features that the target wants as payload, otherwise don't need
            // to send any payload with the signal.
            if (interrupt_context.type != INTERRUPT_TYPE_DEBUGGER_REQ) {
                signal_debugger_with_data(interrupt_context.features);
            } else {
                signal_debugger();
            }
#else // !CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE
            signal_debugger();
#endif // !CONFIG_ENABLE_TARGET_SIDE_DEBUG_MODE

            if (interrupt_context.features & DEBUG_MODE_INTERACTIVE) {
                debug_main();
                // debug loop exited (due to UART cmd to exit debugger), release debugger
                set_state(STATE_SUSPENDED); // sleep and wait for debugger to restore energy
                signal_debugger(); // tell debugger we have shutdown UART
            } // else: exit the ISR, let the app continue in tethered mode
            break;
        case STATE_SUSPENDED: // debugger finished restoring the energy level
            set_state(STATE_IDLE); // return to the application code
            break;
        default:
            // received an unexpected signal from the debugger
            break;
    }
}

void edb_init()
{
    // these pins report state of the debugger state machine on the target
#ifdef CONFIG_STATE_PINS
    GPIO(PORT_STATE, OUT) &= ~(BIT(PIN_STATE_0) | BIT(PIN_STATE_1)); // output low
    GPIO(PORT_STATE, DIR) |= BIT(PIN_STATE_0) | BIT(PIN_STATE_1); // output
#endif

#ifdef CONFIG_EVENT_PINS
    GPIO(PORT_EVENT, OUT) &= ~(BIT(PIN_EVENT_0) | BIT(PIN_EVENT_1)); // output low
    GPIO(PORT_EVENT, DIR) |= BIT(PIN_EVENT_0) | BIT(PIN_EVENT_1); // output
#endif

    GPIO(PORT_SIG, DIR) &= ~BIT(PIN_SIG); // input
    GPIO(PORT_SIG, IFG) &= ~BIT(PIN_SIG); // clear interrupt flag (might have been set by the above)

#ifdef LED_IN_DEBUG_STATE
    GPIO(PORT_DEBUG_MODE_LED, OUT) &= ~(BIT(PIN_DEBUG_MODE_LED));
    GPIO(PORT_DEBUG_MODE_LED, DIR) |= BIT(PIN_DEBUG_MODE_LED);
#endif

    // Codepoint pin config must be after the boot breakpoint since may change dir

#if defined(CONFIG_ENABLE_PASSIVE_BREAKPOINTS) || defined(CONFIG_ENABLE_WATCHPOINTS)
    // codepoint pins are outputs
    GPIO(PORT_CODEPOINT, OUT) &= ~BITS_CODEPOINT;
    GPIO(PORT_CODEPOINT, DIR) |= BITS_CODEPOINT;
#elif !defined(CONFIG_STATE_PINS) // codepoint pins are inputs for target-side breakpoints

#ifdef CONFIG_PULL_DOWN_ON_CODEPOINT_LINES
    // TODO: does this drain substantial amount of energy?
    // NOTE: this does not fix the level-shifter "charging up" problem, because
    // the problem is on EDB side: the level-shifter thinks it is driven, when
    // EDB goes into high-Z state. But, EDB has to go into high-Z, else if
    // it drives codepoint pins low, this causes large energy drain.
    GPIO(PORT_CODEPOINT, REN) |= BITS_CODEPOINT; // pull-down
    GPIO(PORT_CODEPOINT, OUT) &= ~BITS_CODEPOINT; // pull-down
#endif // CONFIG_PULL_DOWN_ON_CODEPOINT_LINES

    GPIO(PORT_CODEPOINT, DIR) &= ~BITS_CODEPOINT;
#endif

    set_state(STATE_IDLE);

    // For measuring boot latency
    // GPIO(PORT_STATE, OUT) |= BIT(PIN_STATE_0);

    __enable_interrupt();

    // Check if EDB requested us to enter debug mode
    if ((GPIO(PORT_SIG, IN) & BIT(PIN_SIG)) == BIT(PIN_SIG)) {
        send_interrupted_msg();
    } else {
        // Listen for debugger request to enter debug mode
        unmask_debugger_signal();
    }
}

void edb_set_app_output_cb(app_output_cb_t *cb)
{
    app_output_cb = cb;
}

__attribute__ ((interrupt(GPIO_VECTOR(PORT_SIG))))
void GPIO_ISR(PORT_SIG)(void)
{
	GPIO(PORT_SIG, IFG) &= BIT(PIN_SIG); // clear irrelevant ints

	switch(__even_in_range(INTVEC(PORT_SIG), INTVEC_RANGE(PORT_SIG)))
	{
        case INTFLAG(PORT_SIG, PIN_SIG):
#if 0
            // This is a hack. We use this pin for a different purpose, not
            // related to libedb: to wakeup from sleep. We cannot put this isr
            // outside libedb, in the app, and call a libedb func from it,
            // because we use builtins that only work inside an ISR.
            if (!sig_active) {
                /* Exit from low power state on reti */
                __bic_SR_register_on_exit(LPM3_bits); // LPM3_bits covers all sleep states
                return;
            }
#endif

            // First time the ISR runs, save application stack pointer
            if (state == STATE_IDLE) {
#if defined(__TI_COMPILER_VERSION__) || defined(__IAR_SYSTEMS_ICC__)
                uint16_t *sp = (uint16_t *) __get_SP_register();
#elif defined(__GNUC__)
                uint16_t *sp;
                __asm__ (
                    "mov.w r1, %0\n"
                    : "=r" (sp) /* output */
                    : /* no inputs */
                    : /* no clobbers */
                );

                /* layout: LOWADDR >>> HIGHADDR (see disasm of  this ISR)
                 * LOCALS [1x2] | SAVED REGS [12x2] | SR [1x2] | PC [1x2] */
                pc = *(sp + 1 + 12 + 1); // note: pointer arithmetic
#else
#error Compiler not supported!
#endif
            }

            mask_debugger_signal();

            handle_debugger_signal();

            /* Power state manipulation is required to be inside the ISR */
            switch (state) {
                case STATE_DEBUG: /* IDLE->DEBUG just happend: entered energy guard */
                    // TODO: we also get here on *nested* assert/bkpt -- what happens
                    // in that case? It seems to work (at least for nested assert),
                    // but comments need to address this case. Also, nested bkpt need
                    // to be tested.

                    // We clear the sleep flags corresponding to the sleep on request
                    // to enter debug mode here, and do not touch them in the DEBUG->SUSPENDED
                    // transition because when upon exiting from the guard we will
                    // not be asleep.
                    if (debug_flags & DEBUG_REQUESTED_BY_TARGET) {
                        debug_flags &= ~DEBUG_REQUESTED_BY_TARGET;
                        __bic_SR_register_on_exit(DEBUG_MODE_REQUEST_WAIT_STATE_BITS);
                    }

                    // Leave the debugger signal masked. The next thing that will
                    // happen in the sequence is us signaling the debugger when
                    // we are exiting the energy guard.

                    // Once we return from this ISR, the application continues with
                    // continuous power on and the debugger in DEBUG state.
                    break;

                case STATE_SUSPENDED: /* DEBUG->SUSPENDED just happened */
                    // Before unmasking the signal interrupt, disable interrupts
                    // globally in order to not let the next signal interrupt happen
                    // until we are asleep. Unmasking won't let the interrupt
                    // call the ISR.
                    __disable_interrupt();
                    unmask_debugger_signal();

                    __bis_SR_register(DEBUG_MODE_EXIT_WAIT_STATE_BITS | GIE); // go to sleep

                    // We will get here after the next ISR (IDLE case) returns
                    // because it would have been invoked while we are asleep
                    // on the line above. That is, the IDLE case ISR is nested
                    // within the SUSPENDED case ISR. If this debug mode exit
                    // sequence is happening because the target had requested
                    // debug mode, then the current ISR was invoked while
                    // asleep in request_debug_mode(). In order to wakeup from
                    // that sleep upon returning from this outer ISR (SUSPENDED
                    // case), we need to clear the sleep bits (otherwise, the
                    // MCU will go to sleep when the SR bits are automatically
                    // restored upon return from interrupt).
                    if (debug_flags & DEBUG_REQUESTED_BY_TARGET) {
                        debug_flags &= ~DEBUG_REQUESTED_BY_TARGET;
                        __bic_SR_register_on_exit(DEBUG_MODE_REQUEST_WAIT_STATE_BITS);
                    }
                    // Once we return from this outer ISR, application execution resumes
                    break;

                case STATE_IDLE: /* SUSPENDED->IDLE just happened */

                    // Before unmasking the signal interrupt, disable
                    // interrupts globally in order to not let the next signal
                    // interrupt happen until either we return from this ISR.
                    // Unmasking won't let the interrupt call the ISR.
                    __disable_interrupt();
                    unmask_debugger_signal();

                    // We were sleeping on the suspend line in the case above when
                    // the current ISR got called, so before returning, clear the
                    // sleep flags (otherwise, we would go back to sleep after
                    // returning from this ISR because the SR flags prior to the ISR
                    // call are automatically restored upon return from ISR).
                    __bic_SR_register_on_exit(DEBUG_MODE_EXIT_WAIT_STATE_BITS);

                    // Interrupt are current disabled (by the disable call
                    // before unmasking the signal interrupt). The adding the
                    // GIE flag here re-enables the interrupts only after
                    // return from the current ISR, so that the next signal ISR
                    // (unrelated to current enter-exit debug mode sequence)
                    // doesn't get nested within the current one.
                    __bis_SR_register_on_exit(GIE);

                    // Once we return from this inner ISR we end up in the outer ISR
                    break;
                default: /* nothing to do */
                    break;
            }
            break;
	}
}
#ifdef __clang__
// TODO: is this still necesarry -- was clang fixed since then?
// TODO: make symbolic in terms of PORT_SIG
__attribute__ ((section("__interrupt_vector_port1"),aligned(2)))
void (*__vector_port1)(void) = GPIO_ISR(PORT_SIG);
#endif
