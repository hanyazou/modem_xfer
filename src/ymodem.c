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

#include <modem_xfer.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>

#define REQ  'C'
#define SOH  0x01
#define STX  0x02
#define EOT  0x04
#define ACK  0x06
#define NAK  0x15
#define CAN  0x18

#define BUFSIZE 128
#define SOH_SIZE 128
#define STX_SIZE 1024

static int (*tx)(uint8_t);
static int (*rx)(uint8_t *, int timeout_ms);
static int (*save)(char*, uint32_t, uint8_t*, uint16_t);

//#define DEBUG
//#define DEBUG_VERBOSE

#define  err(args...) do { modem_xfer_printf(MODEM_XFER_LOG_ERROR,   args); } while(0)
#define warn(args...) do { modem_xfer_printf(MODEM_XFER_LOG_WARNING, args); } while(0)
#define info(args...) do { modem_xfer_printf(MODEM_XFER_LOG_INFO,    args); } while(0)
#ifdef DEBUG
#define  dbg(args...) do { modem_xfer_printf(MODEM_XFER_LOG_DEBUG,   args); } while(0)
#else
#define  dbg(args...) do { } while(0)
#endif

static int discard(void)
{
    int res = 0;
    uint8_t rxb;
    while (rx(&rxb, 300) == 1) {
        res++;
    }

    return res;
}

static int recv_bytes(uint8_t *buf, int n, int timeout_ms)
{
    int i;
    int res;

    for (i = 0; i < n; i++) {
        res = rx(&buf[i], timeout_ms);
        if (res == 0) {
            //  time out (there might be no sender)
            return i;
        }
        if (res < 0) {
            // error
            return res;
        }
    }

    return i;
}

static void hex_dump(uint8_t *buf, int n)
{
    int i;

    for (i = 0; i < n; i += 16) {
        dbg("%04X: "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
            i,
            buf[i+0], buf[i+1], buf[i+2], buf[i+3], buf[i+4], buf[i+5], buf[i+6], buf[i+7],
            buf[i+8], buf[i+9], buf[i+10], buf[i+11], buf[i+12], buf[i+13], buf[i+14], buf[i+15],
            isprint(buf[i+0]) ? buf[i+0] : '.', isprint(buf[i+1]) ? buf[i+1] : '.',
            isprint(buf[i+2]) ? buf[i+2] : '.', isprint(buf[i+3]) ? buf[i+3] : '.',
            isprint(buf[i+4]) ? buf[i+4] : '.', isprint(buf[i+5]) ? buf[i+5] : '.',
            isprint(buf[i+6]) ? buf[i+6] : '.', isprint(buf[i+7]) ? buf[i+7] : '.',
            isprint(buf[i+8]) ? buf[i+8] : '.', isprint(buf[i+9]) ? buf[i+9] : '.',
            isprint(buf[i+10]) ? buf[i+10] : '.', isprint(buf[i+11]) ? buf[i+11] : '.',
            isprint(buf[i+12]) ? buf[i+12] : '.', isprint(buf[i+13]) ? buf[i+12] : '.',
            isprint(buf[i+14]) ? buf[i+14] : '.', isprint(buf[i+15]) ? buf[i+15] : '.');
    }
}

static uint16_t crc16(uint16_t crc, const void *buf, unsigned int count)
{
    uint8_t *p = (uint8_t*)buf;
    uint8_t *endp = p + count;

    while (p < endp) {
        crc = (crc >> 8)|(crc << 8);
        crc ^= *p++;
        crc ^= ((crc & 0xff) >> 4);
        crc ^= (crc << 12);
        crc ^= ((crc & 0xff) << 5);
    }

    return crc;
}

int ymodem_receive(uint8_t buf[MODEM_XFER_BUF_SIZE],
                   int (*__tx)(uint8_t), int (*__rx)(uint8_t *, int timeout_ms),
                   int (*__save)(char*, uint32_t, uint8_t*, uint16_t))
{
    int res, retry, files;
    uint8_t first_block;
    uint8_t wait_for_file_name;
    uint8_t seqno;
    uint16_t last_block_size;
    uint16_t crc;
    char file_name[12];
    uint32_t file_offset;
    uint32_t file_offset_committed;
    unsigned long file_size;

    tx = __tx;
    rx = __rx;
    save = __save;
    files = 0;

 recv_file:
    retry = 0;
    seqno = 0;
    first_block = 1;
    wait_for_file_name = 1;

    tx(REQ);
    while (1) {
        /*
         * receive block herader
         */
        if (recv_bytes(buf, 1, 1000) != 1) {
            dbg("%02X: header timeout\n", seqno);
            if (first_block) {
                dbg("%02X: send REQ\n", seqno);
                tx(REQ);
            } else {
                seqno--;
                /* rewind file offset */
                file_offset -= last_block_size;
                file_offset_committed = file_offset;
                dbg("%02X: send NAK\n", seqno);
                tx(NAK);
            }
            if (25 <= ++retry) {
                goto cancel_return;
            }
            continue;
        }
        if (buf[0] == EOT) {
            dbg("%02X: EOT\n", seqno);
            tx(NAK);
            recv_bytes(&buf[0], 1, 1000);
            if (buf[0] != EOT) {
                warn("WARNING: EOT expected but received %02X\n", buf[0]);
            }
            tx(ACK);
            files++;
            goto recv_file;
        }
        if (buf[0] != STX && buf[0] != SOH) {
            dbg("%02X: invalid header %02X\n", seqno, buf[0]);
            goto retry;
        }

        /*
         * receive sequence number
         */
        if (recv_bytes(&buf[1], 2, 300) != 2) {
            dbg("%02X: seqno timeout\n", seqno);
            goto retry;
        }
        dbg("%02X: %02X %02X %02X\n", seqno, buf[0], buf[1], buf[1]);
        if (buf[1] != seqno && buf[2] != ((~seqno) + 1)) {
            dbg("%02X: invalid sequence number\n", seqno);
            goto retry;
        }

        /*
         * receive payload
         */
        crc = 0;
        last_block_size = (buf[0] == STX ? STX_SIZE : SOH_SIZE);
        for (int i = 0; i < (buf[0] == STX ? STX_SIZE/BUFSIZE : SOH_SIZE/BUFSIZE); i++) {
            if (recv_bytes(buf, BUFSIZE, 1000) != BUFSIZE) {
                goto retry;
            }
            dbg("%02X: %d bytes received\n", seqno, BUFSIZE);
            #ifdef DEBUG_VERBOSE
            hex_dump(buf, BUFSIZE);
            #endif
            crc = crc16(crc, buf, BUFSIZE);
            if (wait_for_file_name) {
                memcpy(file_name, buf, sizeof(file_name));
                file_name[sizeof(file_name) - 1] = '\0';
                if (file_name[0]) {
                    buf[BUFSIZE - 1] = '\0';  // fail safe
                    hex_dump(buf, 16);
                    dbg("file info string: %s\n", &buf[strlen((char *)buf) + 1]);
                    if (sscanf((char*)&buf[strlen((char *)buf) + 1], "%lu", &file_size) != 1) {
                        warn("WARNING: unknown file size\n");
                        file_size = 0;
                    }
                }
                file_offset = file_offset_committed = 0;
                wait_for_file_name = 0;
            }
            if (!first_block && (file_size == 0 || file_offset < file_size)) {
                unsigned int n;
                if (file_size != 0 && file_size < file_offset + BUFSIZE) {
                    n = (unsigned int)(file_size - file_offset);
                } else {
                    n = BUFSIZE;
                }
                if (save(file_name, file_offset, buf, n) != 0) {
                    err("filed to save to %s\n", file_name);
                    goto cancel_return;
                }
                file_offset += n;
            }
        }

        /*
         * receive and check CRC
         */
        if (recv_bytes(buf, 2, 1000) != 2) {
            wait_for_file_name = first_block;
            goto retry;
        }
        dbg("%02X: crc16: %04x %s %04x\n", seqno, buf[0] * 256 + buf[1],
            (buf[0] * 256 + buf[1]) == crc ? "==" : "!=", crc);
        if ((buf[0] * 256 + buf[1]) != crc) {
            if (first_block) {
                wait_for_file_name = 1;
            } else
            if (file_offset != file_offset_committed) {
                /* rewind file offset and truncate garbage */
                file_offset = file_offset_committed;
                if (save(file_name, file_offset, NULL, 0) != 0) {
                    err("filed to truncate %s\n", file_name);
                    goto cancel_return;
                }
            }
            goto retry;
        }
        tx(ACK);
        retry = 0;

        /*
         * process received block
         */
        if (first_block) {
            if (file_name[0] == 0x00) {
                info("total %d file%s received\n", files, 1 < files ? "s" : "");
                tx(ACK);
                return 0;
            }
            info("receiving file '%s', %lu bytes\n", file_name, (unsigned long)file_size);
            tx(REQ);
            first_block = 0;
            if (save(file_name, file_offset, NULL, 0) != 0) {
                err("filed to truncate %s\n", file_name);
                goto cancel_return;
            }
        } else {
            dbg("receiving file '%s', offset %lu -> %lu (%lx -> %lx)\n", file_name,
                (unsigned long)file_offset_committed, (unsigned long)file_offset,
                (unsigned long)file_offset_committed, (unsigned long)file_offset);
            file_offset_committed = file_offset;
        }
        seqno++;
        continue;

    retry:
        res = discard();
        dbg("%02X: discard %d bytes and send NAK\n", seqno, res);
        tx(NAK);
        if (5 <= ++retry) {
            goto cancel_return;
        }
    }

    return 0;

 cancel_return:
    info("cancel\n");
    tx(CAN);
    tx(CAN);
    rx(buf, 1000);
    return -1;
}

#if 0
// PIC XC8 compiler does not seem to handle weak symbol correctly
void __attribute__((weak)) modem_xfer_printf(int log_level, const char *format, ...)
{
    volatile int dummy = 1;
}
#endif
