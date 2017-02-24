#include <msp430.h>

#include <libwispbase/wisp-base.h>

#include <stdlib.h>
#include <stdbool.h>

#include "target_comm.h"

#define STDIO_BUF_SIZE (UART_MSG_HEADER_SIZE + IO_PAYLOAD_SIZE)

static uint8_t msg_buf[STDIO_BUF_SIZE];
static uint8_t *msg_payload = &msg_buf[UART_MSG_HEADER_SIZE];

static unsigned stdio_payload_len = 0;

int io_putchar(int c)
{
    unsigned msg_len = 0;
    
    msg_payload[stdio_payload_len++] = (uint8_t)c;

    if (stdio_payload_len == IO_PAYLOAD_SIZE || c == '\n') { // flush on new line or full

        msg_buf[msg_len++] = UART_IDENTIFIER_WISP;
        msg_buf[msg_len++] = WISP_RSP_STDIO;
        msg_buf[msg_len++] = stdio_payload_len;
        msg_buf[msg_len++] = 0; // padding

        msg_len += stdio_payload_len;

        UART_send(msg_buf, msg_len);

        stdio_payload_len = 0;
    }

    return c;
}

static int puts_base(const char *ptr, bool newline)
{
    unsigned msg_len;

    // Since puts always includes a '\n', we always flush
    // Send message chunk by chunk, include the current buffer contents
    while (*ptr != '\0') {
        while (*ptr != '\0' && stdio_payload_len < IO_PAYLOAD_SIZE - 1) {
            msg_payload[stdio_payload_len] = *ptr++;
            stdio_payload_len++;
        }
        if (*ptr == '\0' && newline)
            msg_payload[stdio_payload_len++] = '\n'; // puts semantics

        msg_len = 0;
        msg_buf[msg_len++] = UART_IDENTIFIER_WISP;
        msg_buf[msg_len++] = WISP_RSP_STDIO;
        msg_buf[msg_len++] = stdio_payload_len;
        msg_buf[msg_len++] = 0; // padding

        msg_len += stdio_payload_len;

        UART_send(msg_buf, msg_len);

        stdio_payload_len = 0;
    }

    return 0;
}

int io_puts_no_newline(const char *ptr)
{
    return puts_base(ptr, false /* newline */);
}

int io_puts(const char *ptr)
{
    return puts_base(ptr, true /* newline */);
}

void edb_output_app_data(const uint8_t *ptr, unsigned len)
{
    unsigned msg_len, payload_len = 0;
    unsigned data_len = len;

    while (data_len-- && payload_len < IO_PAYLOAD_SIZE - 1) {
        msg_payload[payload_len] = *ptr++;
        payload_len++;
    }

    msg_len = 0;
    msg_buf[msg_len++] = UART_IDENTIFIER_WISP;
    msg_buf[msg_len++] = WISP_RSP_APP_OUTPUT;
    msg_buf[msg_len++] = payload_len;
    msg_buf[msg_len++] = 0; // padding

    msg_len += payload_len;

    UART_send(msg_buf, msg_len);
}
