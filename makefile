# Where the m68k-amigaos toolchain is installed. The release workflow builds in the
# amigadev/crosstools image, which has it in /opt/m68k-amigaos: make TOOLCHAIN=/opt/m68k-amigaos
TOOLCHAIN ?= /opt/amiga
TOOLPREFIX = $(TOOLCHAIN)/bin/m68k-amigaos-

# Everything built depends on this file: flags, link order and the distribution layout are
# defined here, and make does not notice a changed command on its own.
THIS_MAKEFILE := $(firstword $(MAKEFILE_LIST))
CC    = $(TOOLPREFIX)gcc
#CC    = clang
STRIP = $(TOOLPREFIX)strip

TARGET  = openpci-rtl8029.device
CLIENT_TARGET = client
STAT_TARGET = s2stat
ADF_TARGET = openpci-rtl8029.adf
LHA_TARGET = openpci-rtl8029.lha
ADF_VOLUME = OpenPCI
XDFTOOL = xdftool
# LHa for UNIX from the Amiga toolchain. Not plain "lha": Homebrew's lhasa installs an
# lha command that can only extract.
LHA     = $(dir $(TOOLPREFIX))lha

RELEASE = 0
# PERF=1 builds the driver with per-packet timing counters (see PERF_TIMING in driver.c)
PERF = 0

ifeq ($(RELEASE),1)
	# The driver links without a C library: stop GCC turning loops into memset/memcpy calls
	OPTIMIZE = -O2 -fno-tree-loop-distribute-patterns
	DEBUG    = 
	DEFINES  = 
else
	OPTIMIZE = -O0
	DEBUG    = -g
	DEFINES  = -DPDEBUG
endif

ifeq ($(PERF),1)
	DEFINES += -DPERF_TIMING=1
endif

INCLUDES = -I. -I$(TOOLCHAIN)/m68k-amigaos/ndk-include -I$(TOOLCHAIN)/m68k-amigaos/include
WARNINGS = -Wall -Wwrite-strings -Werror
# -fno-strict-aliasing: the driver moves packet data through buffers as bytes, words and longs
CFLAGS  = -msmall-code -fno-strict-aliasing $(OPTIMIZE) $(DEBUG) $(INCLUDES) $(DEFINES) $(WARNINGS)
LDFLAGS = -nostdlib -msmall-code -fomit-frame-pointer
LIBS    = -ldebug
STRIPFLAGS = 

# Amiga-side tools (client, s2stat) are linked against libnix
TOOL_CFLAGS  = $(CFLAGS) -noixemul
TOOL_LDFLAGS = -noixemul

# Build output: objects per platform and configuration, executables per platform
ifeq ($(RELEASE),1)
	CONFIG = release
else
	CONFIG = debug
endif
ifeq ($(PERF),1)
	CONFIG := $(CONFIG)-perf
endif

AMIGA_OBJ = obj/AmigaOS/$(CONFIG)
AMIGA_BIN = bin/AmigaOS
HOST_OBJ  = obj/macOS
HOST_BIN  = bin/macOS
DIST      = dist

# The executables in bin/ do not say which configuration built them. Record it, so that
# switching configuration relinks them even when that configuration's objects are older.
CONFIG_STAMP = $(AMIGA_BIN)/.config
ifeq ($(filter clean distclean server,$(MAKECMDGOALS)),)
$(shell mkdir -p $(AMIGA_BIN) && (echo $(CONFIG) | cmp -s - $(CONFIG_STAMP) || echo $(CONFIG) > $(CONFIG_STAMP)))
endif

# Add your source files here, do not include the "src/ne2000/" folder name this script will automatically include that.
# header.s must stay first: with -nostdlib the binary starts at the first object's code, and its
# stub (moveq #-1,d0; rts) makes running the device from a Shell return harmlessly.
SRCS = header.s driver.c __divsi3.c __udivsi3.c
OBJS = $(addprefix $(AMIGA_OBJ)/ne2000/,$(SRCS:%=%.o))

CLIENT_OBJS = $(AMIGA_OBJ)/Tools/client.o
STAT_OBJS   = $(AMIGA_OBJ)/Tools/s2stat.o
DEPS = $(OBJS:.o=.d) $(CLIENT_OBJS:.o=.d) $(STAT_OBJS:.o=.d)

# Host-side test server, built with the host compiler
HOST_CC     = cc
HOST_CFLAGS = -Wall -Wextra -O2
SERVER_OBJS = $(HOST_OBJ)/Tools/server.o

.PHONY: all driver client s2stat server adf lha dist bump clean distclean
all: $(AMIGA_BIN)/$(TARGET) $(AMIGA_BIN)/$(CLIENT_TARGET) $(AMIGA_BIN)/$(STAT_TARGET)
driver: $(AMIGA_BIN)/$(TARGET)
client: $(AMIGA_BIN)/$(CLIENT_TARGET)
s2stat: $(AMIGA_BIN)/$(STAT_TARGET)
server: $(HOST_BIN)/server
adf: $(DIST)/$(ADF_TARGET)
lha: $(DIST)/$(LHA_TARGET)
dist: adf lha

-include $(DEPS)

# Fallback for "make clean all", where the stamp is not written above
$(CONFIG_STAMP):
	@mkdir -p $(dir $@)
	@echo $(CONFIG) > $@

$(AMIGA_OBJ)/ne2000/%.c.o: src/ne2000/%.c $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) $<
	$(CC) $(CFLAGS) -c -o $@ $<

# Assembler sources go through the C preprocessor too, for #include "rev.h"
$(AMIGA_OBJ)/ne2000/%.s.o: src/ne2000/%.s $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) -x assembler-with-cpp $<
	$(CC) $(CFLAGS) -x assembler-with-cpp -c -o $@ $<

$(AMIGA_OBJ)/Tools/%.o: src/Tools/AmigaOS/%.c $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(TOOL_CFLAGS) $<
	$(CC) $(TOOL_CFLAGS) -c -o $@ $<

$(AMIGA_BIN)/$(TARGET): $(OBJS) $(CONFIG_STAMP) $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@.debug $(OBJS) $(LIBS)
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

$(AMIGA_BIN)/$(CLIENT_TARGET): $(CLIENT_OBJS) $(CONFIG_STAMP) $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $(CLIENT_OBJS) -lm

$(AMIGA_BIN)/$(STAT_TARGET): $(STAT_OBJS) $(CONFIG_STAMP) $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $(STAT_OBJS) -lm

# Distribution layout, shared by the floppy image and the LhA archive: the installer, guide,
# readme and Licenses at the root, the driver in Devs/Networks and the tools in C, like the system
# directories. It is staged in obj/dist/$(DIST_NAME) and both archives are made from that tree.
DIST_NAME  = openpci-rtl8029
STAGE      = obj/dist/$(DIST_NAME)
INSTALL_FILES = src/ne2000/Install src/ne2000/Install.info
DOC_FILES   = src/dist/openpci-rtl8029.guide src/dist/openpci-rtl8029.guide.info
# Aminet readme: same base name as the archive
README      = src/dist/$(DIST_NAME).readme
# The GPL requires the license and the way to the source to travel with the binaries
DIST_LICENSES = LICENSE licenses/MIT.txt licenses/matay-prm-rtl8029.txt
DIST_INPUTS = $(AMIGA_BIN)/$(TARGET) $(AMIGA_BIN)/$(CLIENT_TARGET) $(AMIGA_BIN)/$(STAT_TARGET) \
              $(INSTALL_FILES) $(DOC_FILES) $(README) $(DIST_LICENSES)

$(STAGE): $(DIST_INPUTS) $(THIS_MAKEFILE)
	rm -rf $@
	mkdir -p $@/C $@/Devs/Networks
	cp $(AMIGA_BIN)/$(TARGET) $@/Devs/Networks/
	cp $(AMIGA_BIN)/$(CLIENT_TARGET) $(AMIGA_BIN)/$(STAT_TARGET) $@/C/
	cp $(INSTALL_FILES) $(DOC_FILES) $(README) $@/
	mkdir -p $@/Licenses
	cp LICENSE $@/Licenses/GPL-2.0.txt
	cp licenses/MIT.txt $@/Licenses/MIT.txt
	cp licenses/matay-prm-rtl8029.txt $@/Licenses/Matay-original.txt

# Floppy image (volume $(ADF_VOLUME)) with the staged tree at its root, plus the disk's own
# icon (it also sets where the disk's window opens). Always built from scratch: replacing
# files inside an existing image has produced corrupt copies.
DISK_ICON = src/dist/Disk.info

$(DIST)/$(ADF_TARGET): $(STAGE) $(DISK_ICON)
	@mkdir -p $(dir $@)
	rm -f $@
	$(XDFTOOL) $@ format $(ADF_VOLUME) \
		$$(cd $(STAGE) && find . -mindepth 1 -type d | sort | sed 's|^\./|+ makedir |') \
		$$(cd $(STAGE) && find . -type f | sort | sed 's|^\./\(.*\)|+ write $(STAGE)/\1 \1|') \
		+ write $(DISK_ICON) Disk.info
	$(XDFTOOL) $@ list

# LhA archive with the staged tree in a $(DIST_NAME) drawer, and the drawer's icon next to
# it. -lh5 compression and level 1 headers, which every Amiga LhA and lha.library reads.
DRAWER_ICON = src/dist/$(DIST_NAME).info

$(DIST)/$(LHA_TARGET): $(STAGE) $(DRAWER_ICON) $(README)
	@mkdir -p $(dir $@)
	rm -f $@
	cp $(DRAWER_ICON) $(dir $(STAGE))$(DIST_NAME).info
	cd $(dir $(STAGE)) && $(LHA) -ao5q2 -1 $(abspath $@) $(DIST_NAME) $(DIST_NAME).info
	cp $(README) $(DIST)/   # Aminet expects the readme next to the archive too
	$(LHA) -l $@

$(HOST_OBJ)/Tools/%.o: src/Tools/macOS/%.c $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST_BIN)/server: $(SERVER_OBJS) $(THIS_MAKEFILE)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $(SERVER_OBJS)

# Version and dates: make bump VERSION=2.1 [DATE=dd.mm.yyyy]; without VERSION only the dates
bump:
	@scripts/bump-version.sh "$(VERSION)" "$(DATE)"

# Leaves dist/ alone: a built release image survives a clean
clean:
	rm -rf bin obj

distclean: clean
	rm -rf $(DIST)
