# The client core built for Linux, to test against a server without hardware.
CORE := ../lib/bps_core/src
CFLAGS ?= -O2 -g -Wall -Wextra
override CFLAGS += -std=gnu11 -ffp-contract=off -pthread -I$(CORE)

SRC := host.c ../src/port/sockets.c $(wildcard $(CORE)/*.c)

bps-host-client: $(SRC) $(wildcard $(CORE)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f bps-host-client

.PHONY: clean
