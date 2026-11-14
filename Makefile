# OrgOS - Raspberry Pi 4B, AArch64

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
HOSTCC     ?= cc
HOSTCFLAGS ?= -std=gnu17 -Wall -Wextra -Werror -O2

SRC_DIR := src
OBJ_DIR := obj

TARGET  := kernel8.img
ELF     := $(OBJ_DIR)/kernel8.elf

C_SRCS  := $(wildcard $(SRC_DIR)/*.c)
S_SRCS  := $(wildcard $(SRC_DIR)/*.S)

C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))
S_OBJS := $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.S.o,$(S_SRCS))

OBJS    := $(S_OBJS) $(C_OBJS)
DEPS    := $(OBJS:.o=.d)

ARCHFLAGS := -mcpu=cortex-a72

CFLAGS := \
  -std=gnu17 \
  $(ARCHFLAGS) \
  -ffreestanding \
  -fno-builtin \
  -fno-stack-protector \
  -fno-pic \
  -fno-pie \
  -nostdlib \
  -nostartfiles \
  -Wall -Wextra \
  -O2 \
  -mgeneral-regs-only \
  -mstrict-align \
  -MMD -MP

ASFLAGS := \
  $(ARCHFLAGS) \
  -ffreestanding \
  -fno-pic \
  -fno-pie \
  -MMD -MP

LDFLAGS := -T link.ld

.PHONY: all clean check

all: $(TARGET)

$(TARGET): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) link.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.S.o: $(SRC_DIR)/%.S | $(OBJ_DIR)
	$(CC) $(ASFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

check: | $(OBJ_DIR)
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/midi_output_test.c $(SRC_DIR)/midi_output.c \
		$(SRC_DIR)/usb_midi.c -o $(OBJ_DIR)/midi_output_test
	$(OBJ_DIR)/midi_output_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/midi_ports_test.c $(SRC_DIR)/midi_ports.c \
		-o $(OBJ_DIR)/midi_ports_test
	$(OBJ_DIR)/midi_ports_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/midi_port_io_test.c $(SRC_DIR)/midi_port_io.c \
		$(SRC_DIR)/midi_ports.c $(SRC_DIR)/midi_input.c \
		$(SRC_DIR)/midi_usb_bridge.c $(SRC_DIR)/midi_stream.c \
		-o $(OBJ_DIR)/midi_port_io_test
	$(OBJ_DIR)/midi_port_io_test

-include $(DEPS)

