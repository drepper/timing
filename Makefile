.PHONY: all clean

CC = gcc -std=gnu99
CFLAGS = $(OPT) $(DEBUG) $(WARN)
OPT = -O2
DEBUG = -g
WARN = -Wall
LDLIBS = -lgmp

all: timing

cpuid: timing.c

clean:
	-rm -f timing
