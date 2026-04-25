# Host-runnable test suite. No ESP-IDF, no chip — just gcc.
# Tests the wire protocol (phy_rpc_proto.h) only; chip-side behaviour
# is covered by the IDF builds in CI.
#
#   make            build the test runner
#   make test       build and run; exits non-zero on failure
#   make clean      drop build/

ROOT      := $(abspath ../..)
PROTO_DIR := $(ROOT)/host/include
BUILD     := build

CC        ?= cc
CFLAGS    := -std=c99 -g -O0 -Wall -Wextra -Wno-unused-parameter -I$(PROTO_DIR)
LDFLAGS   :=

TEST_SRCS := test_main.c test_proto.c
TEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRCS))

$(BUILD)/test_runner: $(TEST_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BUILD)/test_runner
	$(BUILD)/test_runner

clean:
	rm -rf $(BUILD)

.PHONY: test clean
