# Build with devkitPro (devkitARM + libctru + citro2d + citro3d).
# Run `make` from the "devkitPro MSYS2" / "MSYS2 MinGW" shell that the devkitPro installer provides.
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Open the devkitPro (MSYS2) terminal, or: export DEVKITARM=/opt/devkitpro/devkitARM")
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO is not set. Example: export DEVKITPRO=/opt/devkitpro")
endif

include $(DEVKITARM)/3ds_rules

TARGET  := afterbeat_3ds
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
LIBDIRS := $(DEVKITPRO)/libctru $(DEVKITPRO)/portlibs/3ds
INCLUDE := $(foreach d,$(LIBDIRS),-I$(d)/include)
LIBPATH := $(foreach d,$(LIBDIRS),-L$(d)/lib)

CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections -std=gnu99 $(ARCH) -D__3DS__ $(INCLUDE)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
LIBS    := -lcitro2d -lcitro3d -lctru -lm

all: $(TARGET).3dsx

$(TARGET).elf: afterbeat_3ds.c
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LIBPATH) $(LIBS) -o $@

clean:
	rm -f $(TARGET).elf $(TARGET).3dsx $(TARGET).map
