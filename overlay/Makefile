#---------------------------------------------------------------------------------
# Chirp's overlay (libultrahand: libtesla plus its utilities, in
# ../vendor/libultrahand). Builds chirp.ovl: it goes on the card as
# switch/.overlays/chirp.ovl, for Ultrahand's menu. ../build.ps1 builds it.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE	:= Chirp
APP_AUTHOR	:= Claude
APP_VERSION	:= 1.0.0
TARGET		:= chirp
BUILD		:= build
ULTRA		:= ../vendor/libultrahand
SOURCES		:= source ../common $(ULTRA)/common $(ULTRA)/libultra/source $(ULTRA)/libtesla/source
INCLUDES	:= source ../common $(ULTRA)/common $(ULTRA)/libultra/include $(ULTRA)/libtesla/include
NO_ICON		:= 1

#---------------------------------------------------------------------------------
# options for code generation (as libultrahand's example has them)
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -Wall -Os -ffunction-sections -fdata-sections -flto \
          -fuse-linker-plugin -fomit-frame-pointer -finline-small-functions \
          -fno-strict-aliasing -frename-registers -falign-functions=16 \
          $(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION="\"$(APP_VERSION)\""
CFLAGS += -ffile-prefix-map=$(TOPDIR)/= -ffile-prefix-map=$(shell cygpath -m $(TOPDIR) 2>/dev/null)/=

# theme.ini / wallpaper for this overlay alone go in Chirp's own folder (libtesla makes this folder each
# time the overlay opens: /switch/chirp/ here kept coming back on the card)
UI_OVERRIDE_PATH := /config/sys-chirp/
CFLAGS += -DUI_OVERRIDE_PATH="\"$(UI_OVERRIDE_PATH)\""
# (devkitPro's msys2 would turn that /config/... into a Windows path)
export MSYS2_ARG_CONV_EXCL := -DUI_OVERRIDE_PATH

CXXFLAGS := $(CFLAGS) -std=c++26 -Wno-dangling-else -ffast-math -fno-unwind-tables -fno-asynchronous-unwind-tables
CXXFLAGS += -fno-exceptions -fno-rtti

ASFLAGS := $(ARCH)
LDFLAGS += -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--as-needed -Wl,--gc-sections

# what libultrahand needs
# (and libpng, for people's pictures)
LIBS := -lpng16 -lcurl -lminizip -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx -lm

NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
CXXFLAGS += -flto=$(NPROC)
LDFLAGS  += -flto=$(NPROC)

#---------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD := $(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD := $(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

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

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)


$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile MAKEFLAGS="$(filter-out -j% -j,$(MAKEFLAGS)) -j"


#---------------------------------------------------------------------------------
clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf


#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all : $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp 
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl)"
	@printf 'ULTR' >> $@
	@printf "Ultrahand signature has been added.\n"


$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o %_bin.h : %.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
