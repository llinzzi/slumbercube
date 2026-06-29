# Host-side smoke tests for SlumberCube. Pure C, no IDF dependency.
# Run: make -C tests test

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
BUILDDIR = build

TESTS = test_wifi_creds test_agent_config

.PHONY: all test clean
all: test

test: $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do \
	    echo "==> running $$t"; \
	    ./$(BUILDDIR)/$$t || exit 1; \
	done

$(BUILDDIR)/test_%: test_%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. $< -o $@

$(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
