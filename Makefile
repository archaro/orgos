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
	$(HOSTCC) $(HOSTCFLAGS) -fno-pie -no-pie -iquote $(SRC_DIR) \
		tests/mmu_vectors_test.c $(SRC_DIR)/mmu_vectors.c \
		$(SRC_DIR)/mmu_vectors_irq.c $(SRC_DIR)/irq.c \
		$(SRC_DIR)/exception_dispatch.c $(SRC_DIR)/mmu_high_test.c \
		$(SRC_DIR)/mmu_activation.c $(SRC_DIR)/mmu_tables.c \
		$(SRC_DIR)/mmu_boot.c -o $(OBJ_DIR)/mmu_vectors_test
	$(OBJ_DIR)/mmu_vectors_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/mmu_high_execution_test.c $(SRC_DIR)/mmu_high_test.c \
		$(SRC_DIR)/mmu_activation.c \
		$(SRC_DIR)/mmu_tables.c $(SRC_DIR)/mmu_boot.c \
		-o $(OBJ_DIR)/mmu_high_execution_test
	$(OBJ_DIR)/mmu_high_execution_test
	$(HOSTCC) $(HOSTCFLAGS) -fno-pie -no-pie -iquote $(SRC_DIR) \
		tests/mmu_high_test.c $(SRC_DIR)/mmu_tables.c $(SRC_DIR)/mmu_boot.c \
		-o $(OBJ_DIR)/mmu_high_test
	$(OBJ_DIR)/mmu_high_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/mmu_activation_test.c $(SRC_DIR)/mmu_activation.c \
		$(SRC_DIR)/mmu_tables.c $(SRC_DIR)/mmu_boot.c \
		-o $(OBJ_DIR)/mmu_activation_test
	$(OBJ_DIR)/mmu_activation_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/mmu_tables_test.c $(SRC_DIR)/mmu_tables.c $(SRC_DIR)/mmu_boot.c \
		-o $(OBJ_DIR)/mmu_tables_test
	$(OBJ_DIR)/mmu_tables_test
	$(HOSTCC) $(HOSTCFLAGS) -fno-pie -no-pie -iquote $(SRC_DIR) \
		tests/el0_context_test.c tests/el0_context_symbols.S \
		$(SRC_DIR)/mmu_vectors.c \
		$(SRC_DIR)/mmu_high_test.c $(SRC_DIR)/mmu_activation.c \
		$(SRC_DIR)/mmu_tables.c $(SRC_DIR)/mmu_boot.c \
		$(SRC_DIR)/el0_stack.c \
		$(SRC_DIR)/exception_dispatch.c \
		-o $(OBJ_DIR)/el0_context_test
	$(OBJ_DIR)/el0_context_test
	$(HOSTCC) $(HOSTCFLAGS) -fno-pie -no-pie -iquote $(SRC_DIR) \
		tests/el0_test.c $(SRC_DIR)/exception_dispatch.c $(SRC_DIR)/el0_stack.c \
		-o $(OBJ_DIR)/el0_test
	$(OBJ_DIR)/el0_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/exception_dispatch_test.c $(SRC_DIR)/exception_dispatch.c \
		-o $(OBJ_DIR)/exception_dispatch_test
	$(OBJ_DIR)/exception_dispatch_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/dma_test.c -o $(OBJ_DIR)/dma_test
	$(OBJ_DIR)/dma_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/page_test.c -o $(OBJ_DIR)/page_test
	$(OBJ_DIR)/page_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/pcie_test.c -o $(OBJ_DIR)/pcie_test
	$(OBJ_DIR)/pcie_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) -c \
		tests/midi_api_compile_test.c -o $(OBJ_DIR)/midi_api_compile_test.o
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
		$(SRC_DIR)/usb_midi.c $(SRC_DIR)/midi_auto.c \
		-o $(OBJ_DIR)/midi_port_io_test
	$(OBJ_DIR)/midi_port_io_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/midi_stream_bridge_test.c $(SRC_DIR)/midi_stream.c \
		$(SRC_DIR)/midi_usb_bridge.c \
		-o $(OBJ_DIR)/midi_stream_bridge_test
	$(OBJ_DIR)/midi_stream_bridge_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/usb_midi_rx_test.c $(SRC_DIR)/usb_midi_rx.c \
		$(SRC_DIR)/usb_midi.c $(SRC_DIR)/midi_input.c \
		$(SRC_DIR)/midi_usb_bridge.c $(SRC_DIR)/midi_stream.c \
		-o $(OBJ_DIR)/usb_midi_rx_test
	$(OBJ_DIR)/usb_midi_rx_test
	$(HOSTCC) $(HOSTCFLAGS) -I$(SRC_DIR) \
		tests/usb_midi_config_test.c $(SRC_DIR)/usb_midi.c \
		-o $(OBJ_DIR)/usb_midi_config_test
	$(OBJ_DIR)/usb_midi_config_test
	$(HOSTCC) $(HOSTCFLAGS) -ffunction-sections -fdata-sections -I$(SRC_DIR) \
		tests/usb_hub_change_test.c -Wl,--gc-sections \
		-o $(OBJ_DIR)/usb_hub_change_test
	$(OBJ_DIR)/usb_hub_change_test
	$(HOSTCC) $(HOSTCFLAGS) -ffunction-sections -fdata-sections -iquote $(SRC_DIR) \
		tests/midi_verify_test.c -Wl,--gc-sections -o $(OBJ_DIR)/midi_verify_test
	$(OBJ_DIR)/midi_verify_test

	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/console_dma_test.c -o $(OBJ_DIR)/console_dma_test
	$(OBJ_DIR)/console_dma_test
	$(HOSTCC) $(HOSTCFLAGS) -iquote $(SRC_DIR) \
		tests/console_pci_test.c -o $(OBJ_DIR)/console_pci_test
	$(OBJ_DIR)/console_pci_test

-include $(DEPS)

