# Host-runnable test suite. No ESP-IDF, no chip — just gcc.
#
#   make            build the test runner
#   make test       build and run; exits non-zero on failure
#   make clean      drop build/

ROOT      := $(abspath ../..)
PROTO_DIR := $(ROOT)/esp_hosted_open/include
DISP_SRC  := $(ROOT)/esp_hosted_open/src/phy_rpc.c
SHIM_DIR  := shims
BUILD     := build

CC        ?= cc
CFLAGS    := -std=gnu99 -g -O0 -Wall -Wextra -Wno-unused-parameter \
             -I$(PROTO_DIR) -I$(SHIM_DIR)
LDFLAGS   := -lpthread

TEST_SRCS := test_main.c test_proto.c test_dispatcher.c mock_esp_hosted.c
TEST_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRCS))

DISP_OBJ  := $(BUILD)/phy_rpc.o

$(BUILD)/test_runner: $(TEST_OBJS) $(DISP_OBJ)
	@mkdir -p $(@D)
	$(CC) -o $@ $(TEST_OBJS) $(DISP_OBJ) $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(DISP_OBJ): $(DISP_SRC)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BUILD)/test_runner
	$(BUILD)/test_runner

clean:
	rm -rf $(BUILD)

.PHONY: test clean
