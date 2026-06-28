# Host-runnable unit tests for PCF85063.
# Builds with system cc — no ESP-IDF required.
#
# Usage:
#   make test     - compile and run
#   make clean    - remove build artefacts

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O2 -g
INCLUDE := -I../components/pcf85063/include

SRCS    := test_pcf85063_bcd.c \
           ../components/pcf85063/pcf85063_bcd.c

BIN     := test_pcf85063_bcd

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)