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

//#define DEBUG
//#define DEBUG_VERBOSE

#include "modem_xfer_debug.h"
#include "ymodem.h"

int ymodem_receive(uint8_t buf[MODEM_XFER_BUF_SIZE])
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

    files = 0;

 recv_file:
    retry = 0;
    seqno = 0;
    first_block = 1;
    wait_for_file_name = 1;

    modem_xfer_tx(REQ);
    while (1) {
        /*
         * receive block herader
         */
        if (modem_xfer_recv_bytes(buf, 1, 1000) != 1) {
            dbg("%02X: header timeout\n", seqno);
            if (first_block) {
                dbg("%02X: send REQ\n", seqno);
                modem_xfer_tx(REQ);
            } else {
                seqno--;
                /* rewind file offset */
                file_offset -= last_block_size;
                file_offset_committed = file_offset;
                dbg("%02X: send NAK\n", seqno);
                modem_xfer_tx(NAK);
            }
            if (25 <= ++retry) {
                goto cancel_return;
            }
            continue;
        }
        if (buf[0] == EOT) {
            dbg("%02X: EOT\n", seqno);
            modem_xfer_tx(NAK);
            modem_xfer_recv_bytes(&buf[0], 1, 1000);
            if (buf[0] != EOT) {
                warn("WARNING: EOT expected but received %02X\n", buf[0]);
            }
            modem_xfer_tx(ACK);
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
        if (modem_xfer_recv_bytes(&buf[1], 2, 300) != 2) {
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
        for (int i = 0; i < last_block_size/BUFSIZE; i++) {
            int n = modem_xfer_recv_bytes(buf, BUFSIZE, 1000);
            if (n != BUFSIZE) {
                info("%02X: payload %d timeout, n=%d\n", seqno, i, n);
                goto retry;
            }
            dbg("%02X: %d bytes received\n", seqno, BUFSIZE);
            #ifdef DEBUG_VERBOSE
            hex_dump(buf, BUFSIZE);
            #endif
            crc = modem_xfer_crc16(crc, buf, BUFSIZE);
            if (wait_for_file_name) {
                memcpy(file_name, buf, sizeof(file_name));
                file_name[sizeof(file_name) - 1] = '\0';
                if (file_name[0]) {
                    buf[BUFSIZE - 1] = '\0';  // fail safe
                    modem_xfer_hex_dump(buf, 16);
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
                res = modem_xfer_save(file_name, file_offset, buf, n);
                if (res != 0) {
                    err("failed to save to %s, %d\n", file_name, res);
                    goto cancel_return;
                }
                file_offset += n;
            }
        }

        /*
         * receive and check CRC
         */
        if (modem_xfer_recv_bytes(buf, 2, 1000) != 2) {
            err("%02X: CEC timeout\n", seqno);
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
                res = modem_xfer_save(file_name, file_offset, NULL, 0);
                if (res != 0) {
                    err("failed to truncate %s, %d\n", file_name, res);
                    goto cancel_return;
                }
            }
            goto retry;
        }
        modem_xfer_tx(ACK);
        retry = 0;

        /*
         * process received block
         */
        if (first_block) {
            if (file_name[0] == 0x00) {
                info("total %d file%s received\n", files, 1 < files ? "s" : "");
                modem_xfer_tx(ACK);
                return 0;
            }
            info("receiving file '%s', %lu bytes\n", file_name, (unsigned long)file_size);
            modem_xfer_tx(REQ);
            first_block = 0;
            res = modem_xfer_save(file_name, file_offset, NULL, 0);
            if (res != 0) {
                err("failed to truncate %s, %d\n", file_name, res);
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
        res = modem_xfer_discard();
        dbg("%02X: discard %d bytes and send NAK\n", seqno, res);
        modem_xfer_tx(NAK);
        if (5 <= ++retry) {
            goto cancel_return;
        }
    }

    return 0;

 cancel_return:
    info("cancel\n");
    modem_xfer_tx(CAN);
    modem_xfer_tx(CAN);
    modem_xfer_rx(buf, 1000);
    return -1;
}
