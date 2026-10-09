# The client core built for Linux, to test against a server without hardware.
CORE := ../lib/bps_core/src
CFLAGS ?= -O2 -g -Wall -Wextra
CFLAGS += -std=gnu11 -ffp-contract=off -pthread -I$(CORE)

bps-host-client: host.c $(wildcard $(CORE)/*.c) $(wildcard $(CORE)/*.h)
	$(CC) $(CFLAGS) -o $@ host.c $(wildcard $(CORE)/*.c) -lm

clean:
	rm -f bps-host-client

.PHONY: clean
