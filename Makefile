# Build inside WSL Ubuntu-22.04 (glibc matches the RG35XX Plus stock firmware).
#   make pc   -> build/pc/rg35xx-files   (runs in WSL with a window, for testing)
#   make rg   -> build/rg/rg35xx-files   (aarch64 binary for the device)
#   make dist -> dist/APPS/...           (copy into the SD card's Roms/APPS folder)

SRC := src/main.c src/ui.c src/ftpd.c src/fileops.c
HDR := $(wildcard src/*.h)
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -std=gnu11 -I/usr/include/SDL2 -D_REENTRANT
LIBS := -lSDL2 -lm -lpthread

RG_CC := aarch64-linux-gnu-gcc
RG_CFLAGS := $(CFLAGS) -DRG35XX -mcpu=cortex-a53 -I/usr/include/aarch64-linux-gnu
RG_LDFLAGS := -L/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,/usr/lib/aarch64-linux-gnu

.PHONY: all pc rg dist clean
all: pc rg

pc: build/pc/rg35xx-files
rg: build/rg/rg35xx-files

build/pc/rg35xx-files: $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -o $@ $(SRC) $(LIBS)

build/rg/rg35xx-files: $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	$(RG_CC) $(RG_CFLAGS) -o $@ $(SRC) $(RG_LDFLAGS) $(LIBS)

dist: rg
	@mkdir -p dist/APPS/FilesFTP
	cp build/rg/rg35xx-files dist/APPS/FilesFTP/
	cp package/FilesFTP.sh dist/APPS/
	@echo "dist/APPS is ready to copy into the SD card's Roms/APPS folder"

clean:
	rm -rf build dist
