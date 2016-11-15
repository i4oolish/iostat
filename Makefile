# Makefile for iostat, Linux I/O performance monitoring utility
# (C) 2002-2005 by Zlatko Calusic <zlatko@iskon.hr>

CC = gcc
CFLAGS = -g -O2 -Wall -fomit-frame-pointer
LDFLAGS =

SRC = iostat.c
BIN = iostat

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

install: $(BIN)
	install -d /usr/local/bin /usr/local/man/man8
	install -o root -g root -m 755 -s iostat /usr/local/bin
	install -o root -g root -m 644 iostat.8 /usr/local/man/man8

clean:
	rm -f $(BIN) core
