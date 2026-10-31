#---------------------------------------------------------------------------------------------------------------------
# GBA build: devkitARM + libgba, C++20. The GBA HAL drives the hardware directly (P-8).
#
#   make                 debug ROM capsule_<APP>.gba (ENGINE_ASSERT on)
#   make RELEASE=1       release ROM capsule_<APP>_release.gba (ENGINE_ASSERT compiled out)
#   make APP=hello       which apps/<APP> to build (default: hello)
#
# Paths default to devkitPro's standard install; override on the command line if needed.
#---------------------------------------------------------------------------------------------------------------------
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
LIBGBA    ?= $(DEVKITPRO)/libgba
GBAFIX    ?= $(DEVKITPRO)/tools/bin/gbafix
PYTHON    ?= python3

APP       ?= hello
ROOT      := ../..
ifdef RELEASE
BUILD     := build/$(APP)-release
TARGET    := capsule_$(APP)_release
else
BUILD     := build/$(APP)-debug
TARGET    := capsule_$(APP)
endif

ROMTITLE  := CAPSULE ENG
ROMCODE   := CCEN
ROMMAKER  := 00

PREFIX    := $(DEVKITARM)/bin/arm-none-eabi-
CXX       := $(PREFIX)g++
OBJCOPY   := $(PREFIX)objcopy

#---------------------------------------------------------------------------------------------------------------------
# Sources: the app, engine core, and the GBA HAL. Generated sources live in $(BUILD)/generated.
#---------------------------------------------------------------------------------------------------------------------
SOURCES   := $(ROOT)/apps/$(APP)/main.cpp \
             $(wildcard $(ROOT)/src/core/*.cpp) \
             $(wildcard $(ROOT)/src/input/*.cpp) \
             $(wildcard $(ROOT)/src/text/*.cpp) \
             $(wildcard $(ROOT)/src/seq/*.cpp) \
             $(ROOT)/src/hal/gba/hal_gba.cpp
GENERATED := $(BUILD)/generated/default_font.cpp $(BUILD)/generated/input_map.cpp

OBJECTS   := $(patsubst $(ROOT)/%.cpp,$(BUILD)/%.o,$(SOURCES)) $(GENERATED:.cpp=.o)

#---------------------------------------------------------------------------------------------------------------------
# Flags (P-8, P-10): Thumb code, no RTTI/exceptions, LTO, dead-code stripping.
#---------------------------------------------------------------------------------------------------------------------
ARCH      := -mthumb -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi
CXXFLAGS  := -std=c++20 $(ARCH) -O2 -g -Wall -Wextra -Wpedantic -Wshadow \
             -fno-rtti -fno-exceptions -fno-threadsafe-statics -ffunction-sections -fdata-sections -flto \
             -I$(ROOT)/include -isystem $(LIBGBA)/include -MMD -MP
ifdef RELEASE
CXXFLAGS  += -DNDEBUG
endif
LDFLAGS   := $(ARCH) -specs=gba.specs -flto -Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS      := -L$(LIBGBA)/lib -lgba

#---------------------------------------------------------------------------------------------------------------------
.PHONY: all clean check-toolchain

all: check-toolchain $(TARGET).gba

check-toolchain:
	@test -x "$(CXX)" || { echo "devkitARM not found at $(DEVKITARM). Install devkitPro's gba-dev group or set DEVKITPRO." >&2; exit 1; }
	@test -f "$(LIBGBA)/lib/libgba.a" || { echo "libgba not found at $(LIBGBA). Install devkitPro's gba-dev group or set LIBGBA." >&2; exit 1; }

$(TARGET).gba: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -t"$(ROMTITLE)" -c$(ROMCODE) -m$(ROMMAKER)

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/generated/%.o: $(BUILD)/generated/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# P-1: assets are compiled at build time.
$(BUILD)/generated/default_font.cpp: $(ROOT)/assets/fonts/default_8x8.txt $(ROOT)/tools/font_compiler.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(ROOT)/tools/font_compiler.py $< $@ kDefaultFont

# B-11: this build is the GBA input target.
$(BUILD)/generated/input_map.cpp: $(ROOT)/assets/input/default_input.json $(ROOT)/tools/input_compiler.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(ROOT)/tools/input_compiler.py $< gba $@ kInputMap

clean:
	rm -rf build capsule_*.gba

-include $(OBJECTS:.o=.d)
