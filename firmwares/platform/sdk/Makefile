# PLATFORM native app SDK.
#
#   make -C sdk                 both examples for the device (.papp) and the simulator (.so)
#   make -C sdk APP=path/to/dir one app: every *.cpp / *.c in that folder
#   make -C sdk install SD=/path/to/card   copies the .papp files to <card>/Lab/PLATFORM/Apps
#
# Output: build/sdk/<name>.papp (device) and build/sdk/<name>.so (simulator).
ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
OUT      ?= $(ROOT)/build/sdk
CROSS    ?= arm-none-eabi-
APPS     ?= $(ROOT)/sdk/examples/drone $(ROOT)/sdk/examples/stepseq
SD       ?=
INC      := -I$(ROOT)/sdk/include $(EXTRA_INC)

# Device: Cortex-M4F hard float, no PIC (mkpapp.py relocates absolute words),
# no exceptions / RTTI / thread-safe statics / heap.
ARM_FLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O2 -ffunction-sections -fdata-sections \
             -fno-common -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -DPLAT_DEVICE=1 $(INC)
ARM_CXX   := -std=gnu++17 -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
ARM_LD    := -nostartfiles -nodefaultlibs -Wl,--emit-relocs -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
             -T $(ROOT)/sdk/ld/papp.ld --specs=nano.specs -Wl,--start-group -lm -lc_nano -lgcc -Wl,--end-group
# Simulator: a host shared library.
HOST_FLAGS := -O2 -fPIC -fvisibility=hidden $(INC)
HOST_CXX   := -std=c++17 -fno-exceptions -fno-rtti

HOSTCXX ?= g++
RT_SRCS := $(ROOT)/sdk/runtime/plat_rt.c $(ROOT)/sdk/runtime/plat_rt_io.c $(ROOT)/sdk/runtime/plat_rt_cxx.cpp
name = $(notdir $(1))
srcs = $(wildcard $(1)/*.cpp) $(wildcard $(1)/*.c)

all: $(foreach a,$(APPS),$(OUT)/$(call name,$(a)).papp $(OUT)/$(call name,$(a)).so)

define APP_RULES
$(OUT)/$(call name,$(1)).elf: $(call srcs,$(1)) $(RT_SRCS) $(ROOT)/sdk/ld/papp.ld $(wildcard $(ROOT)/sdk/include/*)
	@mkdir -p $(OUT)/obj/$(call name,$(1))
	set -e; for f in $(filter %.c,$(call srcs,$(1))) $(ROOT)/sdk/runtime/plat_rt.c $(ROOT)/sdk/runtime/plat_rt_io.c; do \
	    $(CROSS)gcc $(ARM_FLAGS) -std=gnu11 $(EXTRA_FLAGS) -c $$$$f -o $(OUT)/obj/$(call name,$(1))/$$$$(basename $$$$f).o; done
	set -e; for f in $(filter %.cpp,$(call srcs,$(1))) $(ROOT)/sdk/runtime/plat_rt_cxx.cpp; do \
	    $(CROSS)g++ $(ARM_FLAGS) $(ARM_CXX) $(EXTRA_FLAGS) -c $$$$f -o $(OUT)/obj/$(call name,$(1))/$$$$(basename $$$$f).o; done
	$(CROSS)g++ $(ARM_FLAGS) $(OUT)/obj/$(call name,$(1))/*.o $(ARM_LD) -o $$@
$(OUT)/$(call name,$(1)).papp: $(OUT)/$(call name,$(1)).elf $(ROOT)/tools/mkpapp.py
	python3 -I $(ROOT)/tools/mkpapp.py $$< $$@
$(OUT)/$(call name,$(1)).so: $(call srcs,$(1)) $(wildcard $(ROOT)/sdk/include/*)
	@mkdir -p $(OUT)
	$(HOSTCXX) $(HOST_FLAGS) $(HOST_CXX) $(EXTRA_FLAGS) -shared $(call srcs,$(1)) -o $$@
endef
$(foreach a,$(APPS),$(eval $(call APP_RULES,$(a))))

# simulator libraries only (no ARM toolchain needed)
host: $(foreach a,$(APPS),$(OUT)/$(call name,$(a)).so)

install: all
	@test -n "$(SD)" || { echo "usage: make -C sdk install SD=/path/to/card"; exit 1; }
	mkdir -p "$(SD)/Lab/PLATFORM/Apps"
	cp $(OUT)/*.papp "$(SD)/Lab/PLATFORM/Apps/"

clean:
	rm -rf $(OUT)
.PHONY: all host install clean
