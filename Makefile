#---------------------------------------------------------------------------------
# pvzultimate_nx -- NativeAOT/MonoGame Android wrapper for Horizon
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := pvzultimate_nx
BUILD       := build
SOURCES     := source
INCLUDES    := source
# No ROMFS: assets are read from the SD card at runtime. Pointing
# --romfsdir at a directory that does not exist is a build-time risk
# for no benefit.

APP_TITLE   := PvZ Ultimate
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0

# icon.jpg in the project root is picked up by switch_rules automatically and
# embedded in the NRO. It must be exactly 256x256 baseline JPEG -- the source
# artwork was 372x372 and was resized rather than shipped as-is, because an
# NRO icon of the wrong dimensions is not rejected, it is simply drawn wrong.
ICON        := icon.jpg

#---------------------------------------------------------------------------------
# Routine logging. 0 for a release build, 1 while working on the port.
#
# debug_log flushes on every line, so each one is an SD card write and a normal
# session produces well over a thousand. With this at 0 the calls are compiled
# out completely -- the compiler still type-checks them, then discards them.
#
# Crash dumps and fatal errors are NOT affected: they write through log_write
# and always land in debug.log. A release build that crashes silently would be
# unsupportable, and a dump costs nothing on a run that does not crash.
#
#   make                 -- release, logging off
#   make DEBUG_LOG=1     -- full logging
#---------------------------------------------------------------------------------
DEBUG_LOG   ?= 0

#---------------------------------------------------------------------------------
# -O2 not -O3: the shim is I/O and dispatch bound, and -O3 makes the fault
# handler's stack walk noticeably less useful during bring-up.
#
# -fno-omit-frame-pointer matters for the same reason -- nx_exception_dump walks
# the x29 chain to symbolize a crash, and without frame pointers you get one
# frame and nothing else.
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# nx_pointer.{c,h} are now in source/, so this is on. Setting it back to 0
# compiles input.c against its local mirror of the header and makes touch a
# no-op -- useful only for isolating an input problem from a rendering one.
NXP_AVAILABLE ?= 1
DEFINES := -DNXP_AVAILABLE=$(NXP_AVAILABLE) -DDEBUG_LOG=$(DEBUG_LOG)

CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections -fno-omit-frame-pointer \
           $(ARCH) $(DEFINES) \
           $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Link order matters: EGL/glapi/drm_nouveau must come before -lnx.
#---------------------------------------------------------------------------------
# Order matters: GLESv2 depends on glapi, glapi on drm_nouveau, everything on
# nx. A static linker resolves left to right, so a dependency listed before its
# provider stays unresolved.
# -lpng is for nx_pointer's optional custom cursor (<data_dir>/cursor.png).
# It must precede -lz, which it depends on. If the link fails with undefined
# png_* symbols, the portlib is missing:  dkp-pacman -S switch-libpng
LIBS    := -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
           -lpng -lz -lm -lnx

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

#---------------------------------------------------------------------------------
# NRO metadata: title, author, version and icon.
#
# NROFLAGS was never set, so elf2nro was invoked with neither --nacp nor
# --icon. The .nacp was being BUILT -- the .nro rule depends on it -- and then
# never attached, which is why the title and author never appeared anywhere and
# why setting APP_AUTHOR alone would have changed nothing. switch_rules supplies
# the %.nacp and %.nro recipes but leaves these flags to the project, and this
# Makefile was missing that half.
#
# The icon resolution below is the devkitPro convention: an explicit ICON wins,
# then $(TARGET).jpg, then icon.jpg. It must be 256x256 baseline JPEG.
#---------------------------------------------------------------------------------
ifeq ($(strip $(ICON)),)
  icons := $(wildcard *.jpg)
  ifneq (,$(findstring $(TARGET).jpg,$(icons)))
    export APP_ICON := $(TOPDIR)/$(TARGET).jpg
  else
    ifneq (,$(findstring icon.jpg,$(icons)))
      export APP_ICON := $(TOPDIR)/icon.jpg
    endif
  endif
else
  export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
  export NROFLAGS += --icon=$(APP_ICON)
endif

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

# Always link with the C++ driver, even though every source here is C.
# switch-mesa (libEGL.a) is C++ internally -- its GLSL compiler uses new/delete
# and parts of libstdc++ -- so linking with $(CC) leaves every `operator new`,
# `operator delete` and `std::__throw_length_error` in mesa unresolved. Using
# $(CXX) as the link driver pulls libstdc++ in. Costs nothing: no C++ is
# compiled, and no exception or RTTI machinery is emitted from our objects.
export LD := $(CXX)

export OFILES    := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
