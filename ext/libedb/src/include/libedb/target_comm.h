#ifndef TARGET_COMM_H
#define TARGET_COMM_H

/**
 * @defgroup    UART_PROTOCOL  UART interface
 * @brief       UART usage
 * @details     The UART message structure looks like this:\n
 *              | Byte                 | Name                  | Description                            |
 *              | -------------------- | --------------------- | -------------------------------------- |
 *              | 0                    | UART identifier       | Identifies the source of the message   |
 *              | 1                    | Message descriptor    | Identifies the message                 |
 *              | 2                    | Length                | Length of the upcoming data            |
 *              | 3 to (2 + length)    | Data                  | Message data (optional)                |
 *
 * @{
 */

/*
 * @brief Size of generic header for all UART messages
 * @details WARNING: duplicated from firmware/uart.h
 */
#define UART_MSG_HEADER_SIZE           4

/**
 * @brief Max standard output data (printf/puts/putchar) or app output data in one message
 */
#define IO_PAYLOAD_SIZE             48


/**
 * @brief Size of application output relayed through EDB to host/ground
 */
#define APP_OUTPUT_SIZE 32

/**
 * @brief A magic value prefix in every message comming from the target
 */
#define UART_IDENTIFIER_WISP			0xF1

/**
 * @brief		Command descriptors sent from the debugger to the WISP.
 */
typedef enum {
    WISP_CMD_GET_PC					= 0x00, //!< get WISP program counter
    WISP_CMD_EXAMINE_MEMORY			= 0x01, //!< examine WISP memory
    WISP_CMD_EXIT_ACTIVE_DEBUG		= 0x02, //!< prepare to exit active debug mode
    WISP_CMD_READ_MEM               = 0x03, //!< read memory contents at an address
    WISP_CMD_WRITE_MEM              = 0x04, //!< read memory contents at an address
    WISP_CMD_BREAKPOINT             = 0x05, //!< enable/disable target-side breakpoint
    WISP_CMD_GET_INTERRUPT_CONTEXT  = 0x06, //!< get reason execution was interrupted
    WISP_CMD_SERIAL_ECHO            = 0x07, //!< send serially encoded data over signal line
    WISP_CMD_GET_APP_OUTPUT         = 0x08, //!< get app data packet
} wisp_cmd_t;

/**
 * @brief		Response descriptors sent from the WISP to the debugger.
 * @{
 */
typedef enum {
    WISP_RSP_ADDRESS                = 0x00, //!< message containing program counter
    WISP_RSP_MEMORY					= 0x01, //!< message containing requested memory content
    WISP_RSP_BREAKPOINT             = 0x02, //!< message acknowledging breakpoint cmd
    WISP_RSP_INTERRUPT_CONTEXT      = 0x03, //!< reason execution was interrupted
    WISP_RSP_SERIAL_ECHO            = 0x04, //!< response to the serial echo request
    WISP_RSP_STDIO                  = 0x05, //!< data from printf
    WISP_RSP_APP_OUTPUT             = 0x06, //!< application output to be relayed to host/ground
    WISP_RSP_INTERRUPTED            = 0x07, //!< target has received interrupt request from debugger
} wisp_rsp_t;

/** @} End UART_PROTOCOL */

#define WISP_CMD_MAX_LEN 16

/**
 * @brief Message length for serial communication on the signal line
 */
#define SIG_SERIAL_NUM_BITS 3

/**
 * @defgroup DEBUG_MODE_FLAGS   Debug mode flags
 * @brief Flags that define functionality in debug mode
 * @details NOTE: must update CONFIG_SIG_SERIAL_NUM_BITS when this list changes
 * NOTE: Can only use LSB, since LSB is used internally on the EDB side.
 * @{
 */
#define DEBUG_MODE_NO_FLAGS         0x00
#define DEBUG_MODE_INTERACTIVE      0x01
#define DEBUG_MODE_WITH_UART        0x02
#define DEBUG_MODE_WITH_I2C         0x04
/** @} End DEBUG_MODE_FLAGS */

#define DEBUG_MODE_FULL_FEATURES    (DEBUG_MODE_INTERACTIVE | DEBUG_MODE_WITH_UART)

/**
 * @defgroup    SIG_SERIAL_BIT_DURATION Signal-line serial protocol bit duration param
 * @brief Interval between bit pulses in serial protocol on the signal line
 *
 * @details The interval is defined on both the debugger and the target: in
 *          terms of cycles of the respective clock.
 *
 *          The encoding code on the target itself takes 4 instructions (excluding the
 *          set+clear pair), but the interrupt latency on the debugger is much
 *          greater and highly variable (because of blocking from other ISRs). This
 *          latency is the bottleneck.
 *
 *          NOTE: Must keep this in sync with the clock choice.
 *
 *          TODO: values for both default clock and the fast (debug mode) clock
 * @{
 */
#define SIG_SERIAL_BIT_DURATION_ON_TARGET       128 // MCLK clock cycles
#define SIG_SERIAL_BIT_DURATION_ON_DEBUGGER     480 // SMCLK cycles
/** @} End SIG_SERIAL_BIT_DURATION */

/**
 * @brief Commands sent over the signal line from target to debugger
 * @details The commands are encoded serially and must be
 *          at most SIG_SERIAL_NUM_BITS long
 */
typedef enum {
    SIG_CMD_NONE                            = 0,
    SIG_CMD_INTERRUPT                       = 1,
    SIG_CMD_EXIT                            = 2,
} sig_cmd_t;

/** @brief Time for target to start listening for debugger's signal after having replied
 *         via UART to the debugger's request for entering debug mode on boot
 *
 *         In practice, the extra instructions are enough to mask this latency, so zero.
 */
#define INTERRUPT_ON_BOOT_LATENCY 0

/** @brief Latency between a code point marker edge and the signal to enter debug mode 
 *
 *  @details This is only relevant for passive breakpoints. The execution on
 *           the target continues after the program counter passes the codepoint
 *           marker. If a breakpoint at this code marker is enabled, then
 *           execution must not continue past the code marker. But, the debugger
 *           cannot read the code marker and react by request debug mode to
 *           be entered imediately (i.e. in less than one target clock cycle).
 *           To prevent the target from executing code after the breakpoint,
 *           each code marker is followed by a delay that exceeds debugger's
 *           reaction latency.
 */
#define ENTER_DEBUG_MODE_LATENCY_CYCLES 200

/**
 * @brief Time for the target to start listening for debugger's signal
 *
 * @details This applies to *nested* interrupts into debug mode
 *          (asserts/breakpoints). After the target signals the
 *          debugger (with data identifying whether it wants to
 *          enter a nested debug mode or exit the current debug mode),
 *          it takes some cycles before it actually goes to sleep
 *          and enables the interrupt.
 */
#define NESTED_DEBUG_MODE_INTERRUPT_LATENCY_CYCLES 200

/**
 * @brief Time debugger waits for the target to start listening for exit signal
 * @details When exiting debug mode, the target waits for debugger
 *          to restore the energy level. Once the debugger finishes restoring,
 *          it signals the target. To make sure that by that point in time
 *          the target is already listening for the signal, the debbugger
 *          must wait a little before issuing the signal. Typically, the
 *          restore operation is long enough to cover it, but this is not
 *          guaranteed, and, furthermore, in powered operation mode, the
 *          debugger skips the restore operation.
 *
 *          Units: debugger clock cycles
 */
#define CONFIG_EXIT_DEBUG_MODE_LATENCY_CYCLES 100

/**
 * @brief Duration of the watchpoint pulse
 * @detail Roughly corresponds to the interrupt latency on the debugger.
 *
 *         Measured in target cycles. Depends on target clock.
 *         Verified that at 8 Mhz target, 1 cycle is not enough, 2 is enough.
 */
#define WATCHPOINT_DURATION 32

/**
 * @brief Minimum time between watchpoints
 * @detail Debugger needs this much time to record a watchpoint, during
 *         which it is blind to any more watchpoints.
 *
 *         Measured in target cycles. Depends on target clock>
 */
#define WATCHPOINT_LATENCY 150

/**
 * @brief Reason target execution is interrupted
 */
typedef enum {
    INTERRUPT_TYPE_NONE                     = 0,
    INTERRUPT_TYPE_DEBUGGER_REQ             = 1,
    INTERRUPT_TYPE_TARGET_REQ               = 2,
    INTERRUPT_TYPE_BREAKPOINT               = 3,
    INTERRUPT_TYPE_ENERGY_BREAKPOINT        = 4,
    INTERRUPT_TYPE_ENERGY_GUARD             = 5, // not a true interrupt: execution continues immediately
    INTERRUPT_TYPE_ASSERT                   = 6,
    INTERRUPT_TYPE_BOOT                     = 7,
} interrupt_type_t;

#endif // TARGET_COMM_H
