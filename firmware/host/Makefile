# The client core built for Linux, to test against a server without hardware.
CORE := ../lib/bps_core/src
CFLAGS ?= -O2 -g -Wall -Wextra
override CFLAGS += -std=gnu11 -ffp-contract=off -pthread -I$(CORE)

# The version it reports; .version holds it so that a new commit rebuilds.
GIT_DESCRIBE := $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
override CFLAGS += -DBPS_GIT_DESCRIBE='"$(GIT_DESCRIBE)"'

SRC := host.c ../src/port/sockets.c $(wildcard $(CORE)/*.c)

.version: FORCE
	@echo '$(GIT_DESCRIBE)' | cmp -s - $@ || echo '$(GIT_DESCRIBE)' > $@

bps-host-client: $(SRC) $(wildcard $(CORE)/*.h) .version
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f bps-host-client .version

.PHONY: clean FORCE
