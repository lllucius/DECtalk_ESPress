# SPDX-License-Identifier: MIT
# Copyright (c) 2025 Leland Lucius
#
# Host-native test harness for custom command parser.
# Compiles the parser sources directly (no ESP-IDF required).

CC     ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -g -O0
CFLAGS += -I../main
# Compile without ESP_PLATFORM so the code uses printf fallbacks
# and includes all handlers (no Kconfig guards).

CMD_SRCS = test_custom_commands.c \
           ../main/custom_commands.c \
           ../main/custom_actions.c

DSP_SRCS = test_tlv320_dsp.c \
           ../main/tlv320_dsp.c

CMD_TARGET = test_custom_commands
DSP_TARGET = test_tlv320_dsp

.PHONY: all clean test

all: $(CMD_TARGET) $(DSP_TARGET)

$(CMD_TARGET): $(CMD_SRCS)
	$(CC) $(CFLAGS) -o $@ $(CMD_SRCS)

$(DSP_TARGET): $(DSP_SRCS)
	$(CC) $(CFLAGS) -o $@ $(DSP_SRCS) -lm

test: $(CMD_TARGET) $(DSP_TARGET)
	./$(CMD_TARGET)
	./$(DSP_TARGET)

clean:
	rm -f $(CMD_TARGET) $(DSP_TARGET)
