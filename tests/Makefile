# Host-side smoke tests for SlumberCube. Pure C, no IDF dependency.
# Run: make -C tests test

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
BUILDDIR = build

TESTS = test_wifi_creds test_agent_config \
        test_wake_fsm test_sys_fsm test_net_fsm test_audio_fsm test_display_fsm \
        test_event_router

.PHONY: all test clean
all: test

test: $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do \
	    echo "==> running $$t"; \
	    ./$(BUILDDIR)/$$t || exit 1; \
	done

# 单文件测试 (现有 test_*, 内联 mock 数据)
$(BUILDDIR)/test_wifi_creds: test_wifi_creds.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. $< -o $@

$(BUILDDIR)/test_agent_config: test_agent_config.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. $< -o $@

# FSM 测试:同时编译 test_<name>_fsm.c 和对应生产 ../main/regions/<name>_fsm.c
# 这样测试链接生产代码(不是镜像复制),生产代码漂移会在编译期暴露。
MAIN_DIR  = ../main
REGIONS   = $(MAIN_DIR)/regions
INCLUDES  = -I. -I$(MAIN_DIR) -I$(REGIONS)

$(BUILDDIR)/test_wake_fsm: test_wake_fsm.c $(REGIONS)/wake_fsm.c $(REGIONS)/../app_fsm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(REGIONS)/wake_fsm.c -o $@

$(BUILDDIR)/test_sys_fsm: test_sys_fsm.c $(REGIONS)/sys_fsm.c $(REGIONS)/../app_fsm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(REGIONS)/sys_fsm.c -o $@

$(BUILDDIR)/test_net_fsm: test_net_fsm.c $(REGIONS)/net_fsm.c $(REGIONS)/../app_fsm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(REGIONS)/net_fsm.c -o $@

$(BUILDDIR)/test_audio_fsm: test_audio_fsm.c $(REGIONS)/audio_fsm.c $(REGIONS)/../app_fsm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(REGIONS)/audio_fsm.c -o $@

$(BUILDDIR)/test_display_fsm: test_display_fsm.c $(REGIONS)/display_fsm.c $(REGIONS)/../app_fsm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(REGIONS)/display_fsm.c -o $@

# 事件路由器:测试 + router + 所有 region(因为 router.h include 它们)
$(BUILDDIR)/test_event_router: test_event_router.c \
                               $(MAIN_DIR)/event_router.c \
                               $(REGIONS)/wake_fsm.c $(REGIONS)/sys_fsm.c \
                               $(REGIONS)/net_fsm.c $(REGIONS)/audio_fsm.c \
                               $(REGIONS)/display_fsm.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< \
	    $(MAIN_DIR)/event_router.c \
	    $(REGIONS)/wake_fsm.c $(REGIONS)/sys_fsm.c \
	    $(REGIONS)/net_fsm.c $(REGIONS)/audio_fsm.c \
	    $(REGIONS)/display_fsm.c -o $@

$(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
