TOOLPREFIX = /opt/amiga/bin/m68k-amigaos-
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
VERSION = 2.0

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

INCLUDES = -I. -I./include -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include
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

# Add your source files here, do not include the "src/ne2000/" folder name this script will automatically include that
SRCS = driver.c __divsi3.c __udivsi3.c header.s
OBJS = $(addprefix $(AMIGA_OBJ)/ne2000/,$(SRCS:%=%.o))

CLIENT_OBJS = $(AMIGA_OBJ)/Tools/client.o
STAT_OBJS   = $(AMIGA_OBJ)/Tools/s2stat.o
DEPS = $(OBJS:.o=.d) $(CLIENT_OBJS:.o=.d) $(STAT_OBJS:.o=.d)

# Host-side test server, built with the host compiler
HOST_CC     = cc
HOST_CFLAGS = -Wall -Wextra -O2
SERVER_OBJS = $(HOST_OBJ)/Tools/server.o

.PHONY: all driver client s2stat server adf lha dist clean distclean
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

$(AMIGA_OBJ)/ne2000/%.c.o: src/ne2000/%.c
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) $<
	$(CC) $(CFLAGS) -c -o $@ $<

$(AMIGA_OBJ)/ne2000/%.s.o: src/ne2000/%.s
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) $<
	$(CC) $(CFLAGS) -c -o $@ $<

$(AMIGA_OBJ)/Tools/%.o: src/Tools/AmigaOS/%.c
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(TOOL_CFLAGS) $<
	$(CC) $(TOOL_CFLAGS) -c -o $@ $<

$(AMIGA_BIN)/$(TARGET): $(OBJS) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@.debug $(OBJS) $(LIBS)
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

$(AMIGA_BIN)/$(CLIENT_TARGET): $(CLIENT_OBJS) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $(CLIENT_OBJS) -lm

$(AMIGA_BIN)/$(STAT_TARGET): $(STAT_OBJS) $(CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $(STAT_OBJS) -lm

# Distribution layout, shared by the floppy image and the LhA archive: the installer, ReadMe
# and Licenses at the root, the driver in Devs/Networks and the tools in C, like the system
# directories. It is staged in obj/dist/$(DIST_NAME) and both archives are made from that tree.
DIST_NAME  = openpci-rtl8029
STAGE      = obj/dist/$(DIST_NAME)
INSTALL_FILES = src/ne2000/Install src/ne2000/Install.info
# The GPL requires the license and the way to the source to travel with the binaries
DIST_LICENSES = LICENSE licenses/MIT.txt licenses/matay-prm-rtl8029.txt
DIST_INPUTS = $(AMIGA_BIN)/$(TARGET) $(AMIGA_BIN)/$(CLIENT_TARGET) $(AMIGA_BIN)/$(STAT_TARGET) \
              $(INSTALL_FILES) src/dist/ReadMe $(DIST_LICENSES)

$(STAGE): $(DIST_INPUTS)
	rm -rf $@
	mkdir -p $@/C $@/Devs/Networks
	cp $(AMIGA_BIN)/$(TARGET) $@/Devs/Networks/
	cp $(AMIGA_BIN)/$(CLIENT_TARGET) $(AMIGA_BIN)/$(STAT_TARGET) $@/C/
	cp $(INSTALL_FILES) src/dist/ReadMe $@/
	mkdir -p $@/Licenses
	cp LICENSE $@/Licenses/GPL-2.0.txt
	cp licenses/MIT.txt $@/Licenses/MIT.txt
	cp licenses/matay-prm-rtl8029.txt $@/Licenses/Matay-original.txt

# Floppy image (volume $(ADF_VOLUME)) with the staged tree at its root. Always built from
# scratch: replacing files inside an existing image has produced corrupt copies.
$(DIST)/$(ADF_TARGET): $(STAGE)
	@mkdir -p $(dir $@)
	rm -f $@
	$(XDFTOOL) $@ format $(ADF_VOLUME) \
		$$(cd $(STAGE) && find . -mindepth 1 -type d | sort | sed 's|^\./|+ makedir |') \
		$$(cd $(STAGE) && find . -type f | sort | sed 's|^\./\(.*\)|+ write $(STAGE)/\1 \1|')
	$(XDFTOOL) $@ list

# LhA archive with the staged tree in a $(DIST_NAME) drawer. -lh5 compression and level 1
# headers, which every Amiga LhA and lha.library version reads.
$(DIST)/$(LHA_TARGET): $(STAGE)
	@mkdir -p $(dir $@)
	rm -f $@
	cd $(dir $(STAGE)) && $(LHA) -ao5q2 -1 $(abspath $@) $(DIST_NAME)
	$(LHA) -l $@

$(HOST_OBJ)/Tools/%.o: src/Tools/macOS/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST_BIN)/server: $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^

# Leaves dist/ alone: a built release image survives a clean
clean:
	rm -rf bin obj

distclean: clean
	rm -rf $(DIST)
