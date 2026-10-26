# PLATFORM: build a polyend-tracker-lab workstation (another firmware's src/ws)
# as a card-loaded PLATFORM app -- Phase B (MANUAL.md "Stations").
#
#   make -f sdk/station/station.mk FW=../flipper NAME=FLIPPER MEMKB=8192
#        [EXTRA="core/audio/effects.cpp ..."]   core sources the station uses (from PLATFORM's tree)
#        [FW_EXTRA="core/gen/*.cpp ..."]        sources only the station's tree has (KONSOL's core/gen)
#
# Output: build/stations/<name>.papp (device) and <name>.so (simulator).
# The station's own src/ws plus OUR src/lab (the host's lab.h layout, checked
# at run time) and src/core/fmath.cpp + formats/wav.cpp + format_common.cpp.
ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
FW      ?=
NAME    ?= STATION
MEMKB   ?= 8192
EXTRA   ?=
CROSS   ?= arm-none-eabi-
OUT     ?= $(ROOT)/build/stations
lc       = $(shell echo $(1) | tr A-Z a-z)
APP     := $(call lc,$(NAME))
FWDIR   := $(abspath $(FW))
WS_SRCS := $(wildcard $(FWDIR)/src/ws/*.cpp)
CORE    := $(ROOT)/src/core/fmath.cpp $(ROOT)/src/core/formats/wav.cpp $(ROOT)/src/core/formats/format_common.cpp \
           $(addprefix $(ROOT)/src/,$(EXTRA)) $(wildcard $(addprefix $(FWDIR)/src/,$(FW_EXTRA)))
LAB     := $(ROOT)/src/lab/canvas.cpp $(ROOT)/src/lab/shell.cpp
GLUE    := $(ROOT)/sdk/station/station_glue.cpp
# the station's headers first (ws/...), then ours (lab/, core)
INCS    := -I$(FWDIR)/src -I$(ROOT)/src -I$(ROOT)/src/core -I$(FWDIR)/src/core -I$(ROOT)/sdk/include
DEFS    := -DPLAT_STATION_NAME=\"$(NAME)\" -DPLAT_STATION_MEMKB=$(MEMKB) -DPLAT_STATION_HOME=\"Lab/$(NAME)\"
ARM     := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O2 -ffunction-sections -fdata-sections \
           -fno-common -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-rtti \
           -fno-threadsafe-statics -fno-use-cxa-atexit -std=gnu++17 -DPLAT_DEVICE=1 -Wno-psabi \
           -DOT_TARGET_DEVICE=1 -DOT_FASTRUN=1 -DOT_SDRAM_CACHE=1
WRAP    := -Wl,--wrap=_ZN3lab10drawHeaderERNS_6CanvasERKNS_5ThemeERNS_5ShellEPKc
ARM_LD  := $(WRAP) -nostartfiles -nodefaultlibs -Wl,--emit-relocs -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
           -T $(ROOT)/sdk/ld/papp.ld --specs=nano.specs -u _printf_float \
           -Wl,--start-group -lm -lc_nano -lgcc -Wl,--end-group
HOST    := -O2 -fPIC -fvisibility=hidden -std=c++17 -fno-exceptions -fno-rtti
SRCS    := $(WS_SRCS) $(LAB) $(CORE) $(GLUE)

all: $(OUT)/$(APP).papp $(OUT)/$(APP).so $(OUT)/$(APP).info

# rebuild when the parameters change (NAME / MEMKB / sources are compiled in)
STAMP := $(OUT)/$(APP).params
$(shell mkdir -p $(OUT); echo '$(NAME) $(MEMKB) $(EXTRA) $(FW_EXTRA) $(FWDIR)' | cmp -s - $(STAMP) || \
        echo '$(NAME) $(MEMKB) $(EXTRA) $(FW_EXTRA) $(FWDIR)' > $(STAMP))

$(OUT)/$(APP).elf: $(STAMP) $(lastword $(MAKEFILE_LIST)) $(ROOT)/sdk/ld/papp.ld $(SRCS) $(ROOT)/sdk/runtime/plat_rt.c $(ROOT)/sdk/runtime/plat_rt_cxx.cpp $(ROOT)/sdk/runtime/plat_rt_io.c
	@test -n "$(FW)" || { echo "usage: make -f sdk/station/station.mk FW=../<fw> NAME=<NAME> MEMKB=<kb>"; exit 1; }
	@rm -rf $(OUT)/obj-$(APP) && mkdir -p $(OUT)/obj-$(APP)
	set -e; for f in $(SRCS) $(ROOT)/sdk/runtime/plat_rt_cxx.cpp; do \
	    $(CROSS)g++ $(ARM) $(INCS) $(DEFS) -c $$f -o $(OUT)/obj-$(APP)/$$(echo $$f | md5sum | cut -c1-8)-$$(basename $$f).o; done
	set -e; for f in $(ROOT)/sdk/runtime/plat_rt.c $(ROOT)/sdk/runtime/plat_rt_io.c; do \
	    $(CROSS)gcc $(filter-out -std=gnu++17 -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit,$(ARM)) -std=gnu11 \
	    -c $$f -o $(OUT)/obj-$(APP)/$$(basename $$f).o; done
	$(CROSS)g++ $(ARM) $(OUT)/obj-$(APP)/*.o $(ARM_LD) -o $@
$(OUT)/$(APP).papp: $(OUT)/$(APP).elf $(ROOT)/tools/mkpapp.py
	python3 -I $(ROOT)/tools/mkpapp.py $< $@
	rm -f $(OUT)/$(APP).info

# The station's own tagline and theme accent into both launcher records.
$(OUT)/station_info: $(ROOT)/tools/station_info.cpp
	g++ -O1 $< -ldl -o $@
$(OUT)/$(APP).info: $(OUT)/$(APP).papp $(OUT)/$(APP).so $(OUT)/station_info $(ROOT)/tools/patch_info.py
	$(OUT)/station_info $(OUT)/$(APP).so > $@.tmp
	python3 -I $(ROOT)/tools/patch_info.py $(OUT)/$(APP).papp --desc "$$(sed -n 1p $@.tmp)" --color "$$(sed -n 2p $@.tmp)"
	python3 -I $(ROOT)/tools/patch_info.py $(OUT)/$(APP).so --desc "$$(sed -n 1p $@.tmp)" --color "$$(sed -n 2p $@.tmp)"
	mv $@.tmp $@
$(OUT)/$(APP).so: $(STAMP) $(SRCS)
	@mkdir -p $(OUT)
	g++ $(HOST) $(INCS) $(DEFS) -shared $(WRAP) $(filter-out $(STAMP),$^) -o $@
	rm -f $(OUT)/$(APP).info

.PHONY: all
