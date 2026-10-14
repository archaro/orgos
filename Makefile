# OrgOS - Raspberry Pi 4B, AArch64

CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

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
  -MMD -MP

ASFLAGS := \
  $(ARCHFLAGS) \
  -ffreestanding \
  -fno-pic \
  -fno-pie \
  -MMD -MP

LDFLAGS := -T link.ld

.PHONY: all clean

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

-include $(DEPS)

