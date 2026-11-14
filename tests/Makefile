# Host-side smoke tests for SlumberCube. Pure C, no IDF dependency.
# Run: make -C tests test

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
BUILDDIR = build

TESTS = test_wifi_creds test_agent_config test_alarm_schedule

.PHONY: all test clean
all: test

test: $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do \
	    echo "==> running $$t"; \
	    ./$(BUILDDIR)/$$t || exit 1; \
	done

$(BUILDDIR)/test_%: test_%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. $< -o $@

$(BUILDDIR)/test_alarm_schedule: test_alarm_schedule.c ../main/alarm_schedule.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I../main test_alarm_schedule.c ../main/alarm_schedule.c -o $@

$(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
