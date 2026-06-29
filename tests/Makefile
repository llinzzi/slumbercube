# Host-side smoke tests for SlumberCube. Pure C, no IDF dependency.
# Run: make -C tests test

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
BUILDDIR = build

TESTS = test_wifi_creds

.PHONY: all test clean
all: test

test: $(BUILDDIR)/test_wifi_creds
	@echo "==> running test_wifi_creds"
	@./$(BUILDDIR)/test_wifi_creds

$(BUILDDIR)/test_wifi_creds: test_wifi_creds.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. $< -o $@

$(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)