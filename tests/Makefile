# Host-runnable unit tests.
# Builds with system cc — no ESP-IDF required.
#
# Usage:
#   make           - build all tests
#   make test      - build and run all tests
#   make test_bcd  - build and run only the BCD test
#   make test_time - build and run only the alarm-time-string test
#   make clean     - remove build artefacts

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O2 -g
PCF_INC := -I../components/pcf85063/include
ALARM_INC := -I../components/audio_player_wrapper

.PHONY: all test clean test_bcd test_time

all: test_pcf85063_bcd test_alarm_time_parse

# PCF85063 BCD helpers
test_pcf85063_bcd: test_pcf85063_bcd.c ../components/pcf85063/pcf85063_bcd.c
	$(CC) $(CFLAGS) $(PCF_INC) -o $@ $^

# Alarm time string parser
test_alarm_time_parse: test_alarm_time_parse.c ../components/audio_player_wrapper/alarm_time_parse.c
	$(CC) $(CFLAGS) $(ALARM_INC) -o $@ $^

test: test_pcf85063_bcd test_alarm_time_parse
	./test_pcf85063_bcd
	@echo
	./test_alarm_time_parse

test_bcd: test_pcf85063_bcd
	./test_pcf85063_bcd

test_time: test_alarm_time_parse
	./test_alarm_time_parse

clean:
	rm -f test_pcf85063_bcd test_alarm_time_parse