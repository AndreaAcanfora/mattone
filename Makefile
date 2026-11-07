CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow

CORE = src/cpu.c src/bus.c src/ppu.c src/cart.c
HEADERS = src/gb.h src/internal.h

.PHONY: all test roms clean

all: build/mattone-run

build/mattone-run: $(CORE) src/cli.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(CORE) src/cli.c

roms:
	tests/fetch-roms.sh

test: build/mattone-run roms
	tests/run-tests.sh

clean:
	rm -rf build
