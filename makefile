TOOLPREFIX = /opt/amiga/bin/m68k-amigaos-
CC    = $(TOOLPREFIX)gcc
#CC    = clang
STRIP = $(TOOLPREFIX)strip

TARGET  = openpci-rtl8029.device
VERSION = 2.0

RELEASE = 0

ifeq ($(RELEASE),1)
	OPTIMIZE = -O2
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

# Add your source files here, do not include "ne2000/" folder name this script will automatically include that
SRCS = driver.c __divsi3.c __udivsi3.c header.s
OBJS = $(addprefix obj/,$(SRCS:%=%.o))
DEPS = $(OBJS:.o=.d)

.PHONY: all
all: bin/$(TARGET)

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

.PHONY: clean
clean:
	rm -rf bin obj

