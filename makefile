TOOLPREFIX = /opt/amiga/bin/m68k-amigaos-
CC    = $(TOOLPREFIX)gcc
#CC    = clang
STRIP = $(TOOLPREFIX)strip

TARGET  = openpci-rtl8029.device
CLIENT_TARGET = client
STAT_TARGET = s2stat
VERSION = 2.0

RELEASE = 0

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

INCLUDES = -I. -I./include -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include
WARNINGS = -Wall -Wwrite-strings -Werror
CFLAGS  = -msmall-code $(OPTIMIZE) $(DEBUG) $(INCLUDES) $(DEFINES) $(WARNINGS)
LDFLAGS = -nostdlib -msmall-code -fomit-frame-pointer
LIBS    = -ldebug
STRIPFLAGS = 

# Amiga-side tools (client, s2stat) are linked against libnix. libnix only honours the
# __stack variable in the sources if swapstack.o is linked in; nothing references it, so
# force it with -u.
TOOL_CFLAGS  = $(CFLAGS) -noixemul
TOOL_LDFLAGS = -noixemul -Wl,-u,___stkinit

# Add your source files here, do not include "ne2000/" folder name this script will automatically include that
SRCS = driver.c __divsi3.c __udivsi3.c header.s
OBJS = $(addprefix obj/,$(SRCS:%=%.o))
DEPS = $(OBJS:.o=.d)

# Client source files
CLIENT_SRCS = AmigaOS/src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

.PHONY: all driver client s2stat clean
all: bin/$(TARGET) bin/$(CLIENT_TARGET) bin/$(STAT_TARGET)
s2stat: bin/$(STAT_TARGET)
driver: bin/$(TARGET)
client: bin/$(CLIENT_TARGET)

-include $(DEPS)

obj/%.c.o: ne2000/%.c
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) $<
	$(CC) $(CFLAGS) -c -o $@ $<

obj/%.s.o: ne2000/%.s
	@mkdir -p $(dir $@)
	$(CC) -MM -MP -MT $(@:.o=.d) -MT $@ -MF $(@:.o=.d) $(CFLAGS) $<
	$(CC) $(CFLAGS) -c -o $@ $<

bin/$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@.debug $^ $(LIBS)
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

bin/$(CLIENT_TARGET): $(CLIENT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $^ -lm

bin/$(STAT_TARGET): AmigaOS/src/s2stat.o
	@mkdir -p $(dir $@)
	$(CC) $(TOOL_CFLAGS) $(TOOL_LDFLAGS) -o $@ $^ -lm

%.o: %.c
	$(CC) $(TOOL_CFLAGS) -c -o $@ $<

clean:
	rm -rf bin obj
	rm -f $(CLIENT_OBJS) AmigaOS/src/s2stat.o