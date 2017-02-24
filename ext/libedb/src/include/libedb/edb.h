/**
 * @file
 * @author	Graham Harvey
 * @date	1 May 2015
 * @brief	Prototypes and definitions for debugging functionality on the WISP.
 */

#ifndef LIBEDB_EDB_H
#define LIBEDB_EDB_H

#include <stdint.h>

#include "pin_assign.h"
#include "target_comm.h"

/**
 * @brief Size of the buffer for receiving bytes from the debugger via UART
 *
 * @details A read from the UART (a chunk that could be a part of the message
 *          or multiple messages) goes into this buffer.  Currently the chunk
 *          size is fixed at one byte, so this buffer size is not relevant.
 */
#define CONFIG_DEBUG_UART_BUF_LEN				2

// On these boards target doesn't exit the ISR (or re-enters it
// immediately) -- not sure what exactly is going on, but this
// seems to fix it.
#if defined(BOARD_SPRITE_APP_SOCKET_RHA) || defined(BOARD_SPRITE_APP)
#define UNMASK_DEBUGGER_SIGNAL_WORKAROUND
#endif

// On the EDBv1.0 board the AUX1 and AUX2 signals are routed to pin #n and
// #(n-1) respectively (i.e. the order is reversed). We reverse the index here,
// as a workaround.
#if LIBEDB_BOARD_VERSION_MAJOR == 1 && LIBEDB_BOARD_VERSION_MINOR == 0
#ifdef BOARD_WISP
#define INDEX_TO_PIN(idx) (NUM_CODEPOINT_PINS - idx - 1)
#else // !BOARD_WISP
#define INDEX_TO_PIN(idx) (idx)
#endif // !BOARD_WISP
#else // LIBEDB_BOARD_VERSION != 1.0
#define INDEX_TO_PIN(idx) (idx)
#endif // LIBEDB_BOARD_VERSION

#ifdef CONFIG_ENABLE_PASSIVE_BREAKPOINTS

// The index argument to breakpoint macros identifies a breakpoint *group*.
// All breakpionts in a group can be enabled/disabled together (not separately).

#if defined(CONFIG_PASSIVE_BREAKPOINT_IMPL_C)

#define CODEPOINT(idx) do { \
        GPIO(PORT_CODEPOINT, OUT) = \
            (GPIO(PORT_CODEPOINT, OUT) & ~(BIT(PIN_CODEPOINT_0) | BIT(PIN_CODEPOINT_1))) | \
            (idx << PIN_CODEPOINT_0); \
            __delay_cycles(ENTER_DEBUG_MODE_LATENCY_CYCLES); \
        } while (0)
#define PASSIVE_BREAKPOINT_IMPL(idx) CODEPOINT(idx)

#elif defined(CONFIG_PASSIVE_BREAKPOINT_IMPL_ASM)

// NOTE: The preprocessor is not available inside the inline assembly,
// hence the code duplication and the magic numbers. Port address comes from
// msp430fr5969.h and msp430fr5969.cmd. See this post:
// https://e2e.ti.com/support/development_tools/compiler/f/343/t/433587
//
// What we would like ideally would be:
//    #define CODEPOINT(idx) do {
//        asm ( " BIS.B ((idx + 1) << #PIN_CODEPOINT_0), &PIN_CODEPOINT_REG " );
//        asm ( " BIC.B #PIN_CODEPOINT_0 | #PIN_CODEPOINT_1, &PIN_CODEPOINT_REG " );
//      } while (0)
//    // NOTE: the +1 is because the encoding of all lines low is not allowed, since
//    //       the debugger needs a pulse that would generate an interrupt.
// but we are stuck with approximating the above by the following:

#define CODEPOINT_0 do { \
        asm ( " BIS.B #0x10, &0x0222 " ); \
        asm ( " BIC.B #0x30, &0x0222 "); \
        __delay_cycles(ENTER_DEBUG_MODE_LATENCY_CYCLES); \
    } while (0)
#define CODEPOINT_1 do { \
        asm ( " BIS.B #0x20, &0x0222 " ); \
        asm ( " BIC.B #0x30, &0x0222 "); \
        __delay_cycles(ENTER_DEBUG_MODE_LATENCY_CYCLES); \
    } while (0)
#define CODEPOINT_2 do { \
        asm ( " BIS.B #0x30, &0x0222 " ); \
        asm ( " BIC.B #0x30, &0x0222 "); \
        __delay_cycles(ENTER_DEBUG_MODE_LATENCY_CYCLES); \
    } while (0)

#define PASSIVE_BREAKPOINT_IMPL_INNER(idx) CODEPOINT_ ## idx
#define PASSIVE_BREAKPOINT_IMPL(idx) PASSIVE_BREAKPOINT_IMPL_INNER(idx)

#endif // CONFIG_PASSIVE_BREAKPOINT_IMPL_*

#endif // CONFIG_ENABLE_PASSIVE_BREAKPOINTS

/**
 * @brief Bitmask that stores the enabled/disabled state of internal breakpoints
 */
extern volatile uint16_t _libedb_internal_breakpoints;

void request_debug_mode(interrupt_type_t int_type, unsigned id, unsigned features);
void request_non_interactive_debug_mode();
void request_energy_guard_debug_mode();

#ifdef CONFIG_ENABLE_PASSIVE_BREAKPOINTS
/**
 * @brief Breakpoint which works by having debugger interrupt the target
 * @details The target encodes the breakpoint index as a pulse on dedicated
 *          CODEPOINT GPIO lines. This pulse triggers and interrupt on the
 *          debugger-side. The debugger checks whether breakpoint is enabled
 *          and if so interrupts the target to enter the active debug mode.
 *
 *          The main drawback of this method is the delay necessary after
 *          the breakpoint that covers the latency between the target hitting
 *          the breakpoint and receiving the interrupt to enter active
 *          debug mode. This is on the order of about 100 (target) cycles.
 *          This overhead is incurred even if the breakpoint is disabled.
 */
#define PASSIVE_BREAKPOINT(idx) PASSIVE_BREAKPOINT_IMPL(idx)
#endif // CONFIG_ENABLE_PASSIVE_BREAKPOINTS

/**
 * @brief Breakpoint whose enable/disable state is stored on the target
 * @details The enable/disable state of an internal breakpoint is stored
 *          in target's non-volatile memory. To enable or disable an internal
 *          breakpoint, the target must be in active debug mode and the
 *          debugger must send the respective command over UART. Up to 16 such
 *          breakpoints (with distinct indexes) can exit, limited only by the
 *          bit-width of the mask, which could easily be increased.
 */
#define INTERNAL_BREAKPOINT(idx) \
    if (_libedb_internal_breakpoints & (1 << idx)) \
        request_debug_mode(INTERRUPT_TYPE_BREAKPOINT, idx, DEBUG_MODE_FULL_FEATURES)

#ifndef CONFIG_ENABLE_PASSIVE_BREAKPOINTS
/**
 * @brief Breakpoint whose enable/disable state is controlled by the debugger
 * @details Whether an external breakpoint is enabled or disabled is indicated by the
 *          state of a GPIO line from debugger board to target dedicated to
 *          that one breakpoint index. This line is driven by the debugger.
 *
 *          The advantages over the internal breakpoint are that
 *              (1) the debugger does not need to communicate with the target
 *                  over the UART command interface to enable/disable external
 *                  breakpoints, and
 *              (2) the debugger may condition the breakpoint on the current
 *                  energy level.
 *
 *          The disadvantage is that only as many external breakpoint (with
 *          distinct indexes) as available GPIO lines are supported: currently,
 *          only two (AUX1 and AUX2).
 */
#define EXTERNAL_BREAKPOINT(idx) \
    if (GPIO(PORT_CODEPOINT, IN) & (1 << INDEX_TO_PIN(idx) << PIN_CODEPOINT_0)) \
        request_debug_mode(INTERRUPT_TYPE_BREAKPOINT, idx, DEBUG_MODE_FULL_FEATURES)

#endif // !CONFIG_ENABLE_PASSIVE_BREAKPOINTS

#ifdef CONFIG_ENABLE_WATCHPOINTS
/*
 * @brief Watchpoint for generating an event and reporting it to the host
 * @params  idx     Identifier for the watchpoint (one-based)
 * @details The number of distinct identifiers is (2^NUM_CODEPOINT_PINS - 1)
 */
#define WATCHPOINT(idx) do { \
        GPIO(PORT_CODEPOINT, OUT) = \
            (GPIO(PORT_CODEPOINT, OUT) & ~BITS_CODEPOINT) | WATCHPOINT_MASK(idx); \
        __delay_cycles(WATCHPOINT_DURATION); \
        GPIO(PORT_CODEPOINT, OUT) &= ~WATCHPOINT_MASK(idx); \
        __delay_cycles(WATCHPOINT_LATENCY); \
    } while (0);

#define WATCHPOINT_MASK(idx) (1 << INDEX_TO_PIN(idx) << PIN_CODEPOINT_0)

#else // !CONFIG_ENABLE_WATCHPOINTS
#define WATCHPOINT(idx)
#endif  // !CONFIG_ENABLE_WATCHPOINTS

#define ASSERT(cond) \
    if (!(cond)) request_debug_mode(INTERRUPT_TYPE_ASSERT, __LINE__, DEBUG_MODE_FULL_FEATURES)

#define ENERGY_GUARD_BEGIN() request_energy_guard_debug_mode()
#define ENERGY_GUARD_END() resume_application()

/**
 * @brief	Initialize pins used by the debugger board
 */
void edb_init();

/* @brief Macro for conditionally depending on libedb */
#define EDB_INIT() edb_init()

/**
 * @brief Initiate disconnection from debugger to eventually resume application
 */
void resume_application();


/**
 * @brief Relay a custom app data output packet via EDB
 * @details This is distrinct from STDIO, but similar in its implimentation.
 */
void edb_output_app_data(const uint8_t *ptr, unsigned len);

/**
 * @brief Callback type for delivery a data packet from app to EDB
 */
typedef void (app_output_cb_t)(uint8_t *output, unsigned *len);

/**
 * @brief Set a callback for EDB to call to get data from app
 * @details EDB forwards this data to host, which is useful when
 *          the target does not have a connection to host, and when standard
 *          output is not suitable (e.g. if data is binary).
 */
void edb_set_app_output_cb(app_output_cb_t *cb);

#endif // LIBEDB_EDB_H
