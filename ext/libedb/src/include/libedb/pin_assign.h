#ifndef LIBDEBUG_PIN_ASSIGN_H
#define LIBDEBUG_PIN_ASSIGN_H

#define BIT_INNER(idx) BIT ## idx
#define BIT(idx) BIT_INNER(idx)

#define GPIO_INNER(port, reg) P ## port ## reg
#define GPIO(port, reg) GPIO_INNER(port, reg)

#define INTFLAG_INNER(port, pin) P ## port ## IV_ ## P ## port ## IFG ## pin
#define INTFLAG(port, pin) INTFLAG_INNER(port, pin)

// Ugly workaround to make the pretty GPIO macro work for OUT register
// (a control bit for TAxCCTLx uses the name 'OUT')
#undef OUT

#if defined(BOARD_WISP) || defined(BOARD_MSP_EXP430FR6989)

#ifdef BOARD_WISP

#define PORT_STATE  3
#define PIN_STATE_0 4 // lsb
#define PIN_STATE_1 5 // msb

#elif defined(BOARD_MSP_EXP430FR6989)

#define PORT_STATE  8
#define PIN_STATE_0 4 // lsb
#define PIN_STATE_1 5 // msb

#define PORT_EVENT  8
#define PIN_EVENT_0 6 // lsb
#define PIN_EVENT_1 7 // msb

#endif // BOARD

#define PORT_SIG   1
#define PIN_SIG    4

// Code point pins must be on same port
// NOTE: When using the same pins as PIN_STATE, must disable CONFIG_STATE_PINS
// NOTE: Cannot use macros in inline assembly, so debug.h has these hardcoded!
// NOTE: Codepoint pins must be in order, i.e. CODEPOINT_0 -> pin with lowest index.
#define PORT_CODEPOINT  3
#if defined(BOARD_MSP_EXP430FR6989) // P3.4 and P3.5 are occupied (for serial connection to somewhere)
#define PIN_CODEPOINT_0 3 // lsb
#define PIN_CODEPOINT_1 4 // msb
#else // !BOARD_MSP_EXP430FR6989
#define PIN_CODEPOINT_0 4 // lsb
#define PIN_CODEPOINT_1 5 // msb
#endif // !BOARD_MSP_EXP430FR6989
#define BITS_CODEPOINT  (BIT(PIN_CODEPOINT_0) | BIT(PIN_CODEPOINT_1))
#define NUM_CODEPOINT_PINS 2

#define PORT_DEBUG_MODE_LED 4
#define PIN_DEBUG_MODE_LED  0

#define PORT_DEBUG_LED J
#define PIN_DEBUG_LED  6

#elif defined(BOARD_SPRITE_APP_SOCKET_RHA)

#define PORT_SIG   3
#define PIN_SIG    0

// Code point pins must be on same port and consecutive
// NOTE: When using the same pins as PIN_STATE, must disable CONFIG_STATE_PINS
// NOTE: Cannot use macros in inline assembly, so debug.h has these hardcoded!
#define PORT_CODEPOINT  3
#define PIN_CODEPOINT_0 1 // lsb
#define PIN_CODEPOINT_1 2 // msb
#define PIN_CODEPOINT_2 3 // msb
#define BITS_CODEPOINT  (\
    BIT(PIN_CODEPOINT_0) | \
    BIT(PIN_CODEPOINT_1) | \
    BIT(PIN_CODEPOINT_2))

#define PORT_DEBUG_MODE_LED 1
#define PIN_DEBUG_MODE_LED  0

#elif defined(BOARD_SPRITE_APP)

#define PORT_SIG   3
#define PIN_SIG    0

// Code point pins must be on same port and consecutive
// NOTE: When using the same pins as PIN_STATE, must disable CONFIG_STATE_PINS
// NOTE: Cannot use macros in inline assembly, so debug.h has these hardcoded!
#define PORT_CODEPOINT  3
#define PIN_CODEPOINT_0 1 // lsb
#define PIN_CODEPOINT_1 2 // msb
#define PIN_CODEPOINT_2 3 // msb
#define BITS_CODEPOINT  (\
    BIT(PIN_CODEPOINT_0) | \
    BIT(PIN_CODEPOINT_1) | \
    BIT(PIN_CODEPOINT_2))

#endif // BOARD_*

#endif // LIBDEBUG_PIN_ASSIGN_H
