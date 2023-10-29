/*
 * Copyright (c) 2023 @hanyazou
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __MODEM_XFER_H__
#define __MODEM_XFER_H__

#include <stdint.h>

#define MODEM_XFER_BUF_SIZE 128

enum {
    MODEM_XFER_LOG_ERROR,
    MODEM_XFER_LOG_WARNING,
    MODEM_XFER_LOG_INFO,
    MODEM_XFER_LOG_DEBUG,
    MODEM_XFER_LOG_VERBOSE,
};

extern int ymodem_receive(uint8_t buf[MODEM_XFER_BUF_SIZE],
                          int (*tx)(uint8_t), int (*rx)(uint8_t *, int timeout_ms),
                          int (*save)(char*, uint32_t, uint8_t*, uint16_t));
extern void modem_xfer_printf (int log_level, const char *format, ...)
    __attribute__ ((format (printf, 2, 3)));

#endif  // __MODEM_XFER_H__
