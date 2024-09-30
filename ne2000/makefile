TOOLPREFIX = /opt/amiga/bin/m68k-amigaos-
CC    = $(TOOLPREFIX)gcc
#CC    = clang
STRIP = $(TOOLPREFIX)strip

INCLUDES = -I. -I./include -I/opt/amiga/m68k-amigaos/ndk-include -I/opt/amiga/m68k-amigaos/include
OPTIMIZE = -O2
DEBUG    = -g
DEFINES  = 
WARNINGS = -Wall -Wwrite-strings -Werror

CFLAGS  = $(OPTIMIZE) $(DEBUG) $(INCLUDES) $(DEFINES) $(WARNINGS)
LDFLAGS =  
LIBS    = 

all: header.o driver.o divsi3.o udivsi3.o
	$(CC) -s -nostdlib -msmall-code -o openpci-rtl8029.device\
    header.o driver.o __divsi3.o __udivsi3.o -ldebug

driver.o: driver.c ne2000.h rev.h
	$(CC) -O2 -c -msmall-code -fomit-frame-pointer $(INCLUDES) driver.c

header.o: header.s rev.h
	$(CC) -c header.s

divsi3.o: __divsi3.c
	$(CC) -O2 -c -msmall-code -fomit-frame-pointer $(INCLUDES) __divsi3.c

udivsi3.o: __udivsi3.c
	$(CC) -O2 -c -msmall-code -fomit-frame-pointer $(INCLUDES) __udivsi3.c

.PHONY: clean
clean:
	rm -r header.o driver.o openpci-rtl8029.device
