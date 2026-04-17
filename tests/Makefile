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

SRCS = test_custom_commands.c \
       ../main/custom_commands.c \
       ../main/custom_actions.c

TARGET = test_custom_commands

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
