#
# Makefile
#
#
TARGET = pginsert
CFLAGS = -g -Wall -I/u/postgres95/include
LIBS = -L/u/postgres95/lib -lpq

$(TARGET) : pginsert.o pginterface.o halt.o
	$(CC) -o $(TARGET) $(XFLAGS) $(CFLAGS) \
		 pginsert.o pginterface.o halt.o $(LIBS)

clean:
	rm -f *.o $(TARGET) log core

install:
	make clean
	make CFLAGS=-O
	install -s -o bin -g bin $(TARGET) /usr/local/bin
