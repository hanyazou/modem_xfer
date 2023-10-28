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

#include "modem_xfer.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>
#include <sys/select.h>
#include <string.h>

static int tx_fd = -1;
static int rx_fd = -1;

int open_port(void)
{
    const char *TX = "/tmp/modem_test-tx";
    const char *RX = "/tmp/modem_test-rx";

    mkfifo(TX, 0660);
    tx_fd = open(TX, O_RDWR);
    if(tx_fd < 0) {
        printf("open(%s) failed (errno=%d)\n", TX, errno);
    }
    mkfifo(RX, 0660);
    rx_fd = open(RX, O_RDWR);
    if(rx_fd < 0) {
        printf("open(%s) failed (errno=%d)\n", RX, errno);
    }

    return (0 <= tx_fd) && (0 <= rx_fd) ? 0 : -1;
}

void close_port(void)
{
    if (0 <= tx_fd)
        close(tx_fd);
    if (0 <= rx_fd)
        close(rx_fd);
}

int tx_func(uint8_t c)
{
    int res;
    res = write(tx_fd, &c, 1);
    if (res < 0) {
        return -errno;
    }

    return 1;
}

int rx_func(uint8_t *c, int timeout_ms)
{
    int res;
    fd_set set;
    struct timeval tv;
    FD_ZERO(&set);
    FD_SET(rx_fd, &set);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    res = select(rx_fd + 1, &set, NULL, NULL, &tv);
    if(res < 0) {
        printf("select() failed (errno=%d)\n", errno);
        return -errno;
    }
    if(res == 0) {
        /* timeout occured */
        return 0;
    }

    res = read(rx_fd, c, 1);
    if (res < 0) {
        return -errno;
    }

    return 1;
}

int save_func(char *file_name, uint32_t offset, uint8_t *buf, uint16_t size)
{
    char tmp[12];
    memcpy(tmp, file_name, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    printf("%11s %4u bytes at %6lu\n", tmp, size, (unsigned long)offset);
    return 0;
}

int main(int ac, char *av[])
{
    if (open_port() != 0) {
        printf("open_port() failed\n");
        exit(1);
    }
    if (ymodem_receive(tx_func, rx_func, save_func) != 0) {
        printf("ymodem_receive() failed\n");
    }
    close_port();

    return 0;
}
