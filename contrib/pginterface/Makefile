#
# Makefile
#
#
PGINTERFACE = pginterface.o halt.o 
TARGET = pginsert pgwordcount pgnulltest
CFLAGS = -g -Wall -I/u/postgres95/include 
LDFLAGS = -L/u/postgres95/lib -lpq

all : $(TARGET)

$(TARGET):	$(PGINTERFACE) $*.c
	cc -o $* $(CFLAGS) $*.c $(PGINTERFACE) $(LDFLAGS)

$(PGINTERFACE):	pginterface.c halt.c
	cc -c $(CFLAGS) pginterface.c halt.c
	
clean:
	rm -f *.o $(TARGET) log core

install:
	make clean
	make CFLAGS=-O
	install -s -o bin -g bin $(TARGET) /usr/local/bin
