# 32-bit m1n1 for Apple's ARMv7 chips (A5, A5X, A6, A6X), built with
# make M1N1_ARCH=arm. One binary serves every board: what differs comes from
# the Apple device tree iBoot hands over. clang only.
#
# The processor layer (start-up, exceptions, MMU, timer, Linux hand-off) is
# src/arm32/; the rest is m1n1's shared code. Drivers for hardware these
# chips do not have (hypervisor, NVMe, DCP, DART, ...) are left out.

ifneq ($(USE_CLANG),1)
$(error The 32-bit build needs clang: USE_CLANG=1)
endif

RUSTARCH := armv7a-none-eabi
# m1n1 is position independent. The target's prebuilt core is not, so core and
# alloc are built here as well, with the same relocation model (needs rust-src).
export RUSTFLAGS := -C relocation-model=pic
CARGO_FLAGS := -Z build-std=core,alloc -Z build-std-features=compiler-builtins-mem
ifeq ($(CHAINLOADING),1)
CARGO_FLAGS += --features chainload
endif
ASM_ARCH := armv7-a
CC := $(TOOLCHAIN)clang --target=armv7a-none-eabi
AS := $(TOOLCHAIN)clang --target=armv7a-none-eabi

# u64 is unsigned long long here, and m1n1's printf reads %l as 64 bits on
# this build (vsprintf.c), so its "%lx with a u64" callers stay correct;
# -Wformat cannot express that. Addresses travel as u64 through the shared
# code, hence the two cast warnings.
BASE_CFLAGS := -O2 -Wall -g -Wundef -Werror=strict-prototypes -fno-common -fno-PIE \
	-Werror=implicit-function-declaration -Werror=implicit-int \
	-Wsign-compare -Wunused-parameter -Wno-multichar \
	-ffreestanding -fpic -ffunction-sections -fdata-sections \
	-nostdinc -isystem $(shell $(CC) -print-file-name=include) -isystem sysinc \
	-Isrc \
	-fno-stack-protector -march=armv7-a -marm -mfloat-abi=soft -mno-unaligned-access \
	-DASM_ARCH=\"$(ASM_ARCH)\" \
	-Wno-format -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
	$(EXTRA_CFLAGS)

CFLAGS := $(BASE_CFLAGS)

LDFLAGS := -EL -marmelf --no-undefined -X -Bsymbolic \
	-z notext --orphan-handling=warn \
	-z nocopyreloc --gc-sections -pie

ARM32_OBJECTS := $(patsubst %,arm32/%, \
	a32_start.o \
	a32_startup.o \
	a32_exception.o a32_exception_asm.o \
	a32_memory.o a32_cache_asm.o \
	a32_utils_asm.o \
	a32_cpu.o \
	a32_time.o \
	a32_kboot.o \
	a32_wdt.o \
	a32_storage.o \
	a32_usb.o)

OBJECTS := \
	adt.o \
	bootmgr.o \
	bootlogo_48.o bootlogo_128.o bootlogo_256.o \
	devicetree.o \
	fb.o font.o font_retina.o \
	firmware.o \
	heapblock.o \
	iodev.o \
	main.o \
	payload.o \
	proxy.o \
	ringbuffer.o \
	string.o \
	uart.o \
	uartproxy.o \
	usb_dwc2.o \
	utils.o \
	vsprintf.o \
	$(ARM32_OBJECTS) \
	$(MINILZLIB_OBJECTS) $(TINF_OBJECTS) $(DLMALLOC_OBJECTS) $(LIBFDT_OBJECTS)

FP_OBJECTS :=

BUILD_OBJS := $(patsubst %,build/%,$(OBJECTS))
BUILD_FP_OBJS :=
BUILD_ALL_OBJS := $(BUILD_OBJS) $(BUILD_RUST_LIB)

TARGET_ARMV7 := m1n1-armv7.macho
ALL_TARGETS := build/$(TARGET_ARMV7)
.DEFAULT_GOAL := all
BIN_OBJ_FORMAT := -B arm -O elf32-littlearm

build/$(NAME)-armv7.elf: $(BUILD_ALL_OBJS) m1n1-armv7.ld
	$(QUIET)echo "  LD    $@"
	$(QUIET)$(LD) -T m1n1-armv7.ld $(LDFLAGS) -o $@ $(BUILD_ALL_OBJS)

build/$(NAME)-armv7.macho: build/$(NAME)-armv7.elf
	$(QUIET)echo "  MACHO $@"
	$(QUIET)$(OBJCOPY) -O binary --strip-debug $< $@
