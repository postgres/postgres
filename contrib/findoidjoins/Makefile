#
# Makefile, requires src/interfaces/libpgeasy
#
#
INSTALLDIR = /usr/local/pgsql

TARGET = findoidjoins
PGEASY = ../../src/interfaces/libpgeasy
CFLAGS = -g -Wall -I. -I$(PGEASY) -I$(INSTALLDIR)/include
LIBPGEASY = $(PGEASY)/libpgeasy.a
LDFLAGS = -L$(INSTALLDIR)/lib -lpq

all : $(TARGET)

findoidjoins:	findoidjoins.c $(LIBPGEASY)
	gcc -o $@ $(CFLAGS) $^ $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) log core

install:
	install -s -o bin -g bin $(TARGET) $(INSTALLDIR)/bin

