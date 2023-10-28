
SRC_DIR=../src

all: modem_test

modem_test: modem_test.c $(SRC_DIR)/ymodem.c $(SRC_DIR)//modem_xfer.h
	cc -I$(SRC_DIR) -o modem_test modem_test.c $(SRC_DIR)//ymodem.c

test:: all
	pkill -a modem_test || true
	./modem_test &
	ls -l /tmp/modem_test-*
	sz --ymodem data/foo.txt data/bar.txt data/baz.dat < /tmp/modem_test-tx > /tmp/modem_test-rx 2>/dev/null

clean::
	rm -f modem_test
