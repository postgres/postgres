#
# Makefile
#
#
TARGET = test_fsync
XFLAGS = 
CFLAGS = -O
LIBS = 

$(TARGET) : test_fsync.o
	$(CC) -o $(TARGET) $(XFLAGS) $(CFLAGS) test_fsync.o $(LIBS)

test_fsync.o	: test_fsync.c 
	$(CC) -c $(XFLAGS) $(CFLAGS) test_fsync.c

clean:
	rm -f *.o $(TARGET) log core

install:
	make clean
	make CFLAGS=-O
	install -s -o bin -g bin $(TARGET) /usr/local/bin
