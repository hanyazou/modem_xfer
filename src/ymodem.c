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

#ifdef DEBUG
#define dprintf(args) do { printf args; } while(0)
#else
#define dprintf(args) do { } while(0)
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

    for (i = 0; i < n; i++) {
        if ((i % 16) == 0) {
            printf("%04X:", i);
        }
        printf(" %02X", buf[i]);
        if ((i % 16) == 15) {
            printf("\n");
        }
    }
    if ((i % 16) != 0) {
        printf("\n");
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

int ymodem_receive(int (*__tx)(uint8_t), int (*__rx)(uint8_t *, int timeout_ms),
                   int (*__save)(char*, uint32_t, uint8_t*, uint16_t))
{
    int res, retry, files;
    uint8_t first_block;
    uint8_t wait_for_file_name;
    uint8_t seqno;
    int last_block_size;
    uint8_t buf[3];
    uint8_t tmpbuf[1][BUFSIZE];
    uint8_t *payload = tmpbuf[0];
    uint16_t crc;
    char file_name[12];
    uint32_t file_offset;
    uint32_t file_offset_committed;
    uint32_t file_size;;

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
        if (recv_bytes(buf, 1, 5000) != 1) {
            if (first_block) {
                tx(REQ);
            } else {
                seqno--;
                /* rewind file offset */
                file_offset -= last_block_size;
                file_offset_committed = file_offset;
                tx(NAK);
            }
            if (5 <= ++retry) {
                goto cancel_return;
            }
            continue;
        }
        if (buf[0] == EOT) {
            dprintf(("%02X: EOT\n", seqno));
            tx(NAK);
            recv_bytes(&buf[0], 1, 1000);
            if (buf[0] != EOT) {
                dprintf(("WARNING: EOT expected but received %02X\n", buf[0]));
            }
            tx(ACK);
            files++;
            goto recv_file;
        }
        if (buf[0] != STX && buf[0] != SOH) {
            dprintf(("%02X: invalid header %02X\n", seqno, buf[0]));
            goto retry;
        }

        /*
         * receive sequence number
         */
        if (recv_bytes(&buf[1], 2, 300) != 2) {
            dprintf(("%02X: timeout\n", seqno));
            goto retry;
        }
        dprintf(("%02X: %02X %02X %02X\n", seqno, buf[0], buf[1], buf[1]));
        if (buf[1] != seqno && buf[2] != ((~seqno) + 1)) {
            dprintf(("%02X: invalid sequence number\n", seqno));
            goto retry;
        }

        /*
         * receive payload
         */
        crc = 0;
        last_block_size = (buf[0] == STX ? STX_SIZE : SOH_SIZE);
        for (int i = 0; i < (buf[0] == STX ? STX_SIZE/BUFSIZE : SOH_SIZE/BUFSIZE); i++) {
            if (recv_bytes(payload, BUFSIZE, 1000) != BUFSIZE) {
                goto retry;
            }
            dprintf(("%02X: %d bytes received\n", seqno, BUFSIZE));
            #ifdef DEBUG_VERBOSE
            hex_dump(payload, BUFSIZE);
            #endif
            crc = crc16(crc, payload, BUFSIZE);
            if (wait_for_file_name) {
                memcpy(file_name, payload, sizeof(file_name));
                file_name[sizeof(file_name) - 1] = '\0';
                dprintf(("file info string: %s\n", &payload[sizeof(payload)]));
                payload[BUFSIZE - 1] = '\0';;
                sscanf((char*)&payload[sizeof(payload)], "%u", &file_size);
                file_offset = file_offset_committed = 0;
                wait_for_file_name = 0;
            }
            if (!first_block && file_offset < file_size) {
                int n;
                if (file_size < file_offset + BUFSIZE) {
                    n = file_size - file_offset;
                } else {
                    n = BUFSIZE;
                }
                if (save(file_name, file_offset, payload, n) != 0) {
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
        dprintf(("%02X: crc16: %04x %s %04x\n", seqno, buf[0] * 256 + buf[1],
                 (buf[0] * 256 + buf[1]) == crc ? "==" : "!=", crc));
        if ((buf[0] * 256 + buf[1]) != crc) {
            if (first_block) {
                wait_for_file_name = 1;
            } else
            if (file_offset != file_offset_committed) {
                /* rewind file offset and truncate garbage */
                file_offset = file_offset_committed;
                if (save(file_name, file_offset, NULL, 0) != 0) {
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
                dprintf(("total %d file%s received\n", files, 1 < files ? "s" : ""));
                tx(ACK);
                return 0;
            }
            dprintf(("receiving file '%s', %lu bytes\n", file_name, (unsigned long)file_size));
            tx(REQ);
            first_block = 0;
            if (save(file_name, file_offset, NULL, 0) != 0) {
                goto cancel_return;
            }
        } else {
            dprintf(("receiving file '%s', offset %lu -> %lu (%lx -> %lx)\n", file_name,
                     (unsigned long)file_offset_committed, (unsigned long)file_offset,
                     (unsigned long)file_offset_committed, (unsigned long)file_offset));
            file_offset_committed = file_offset;
        }
        seqno++;
        continue;

    retry:
        res = discard();
        dprintf(("%02X: discard %d bytes\n", seqno, res));
        tx(NAK);
        if (5 <= ++retry) {
            goto cancel_return;
        }
    }

    return 0;

 cancel_return:
    tx(CAN);
    tx(CAN);
    rx(buf, 1000);
    return -1;
}

