#
# Makefile
#
#
TARGET = entab
BINDIR = /usr/local/bin
XFLAGS = 
CFLAGS = -O
LIBS = 

$(TARGET) : entab.o halt.o
	$(CC) -o $(TARGET) $(XFLAGS) $(CFLAGS) entab.o halt.o $(LIBS)

entab.o	: entab.c 
	$(CC) -c $(XFLAGS) $(CFLAGS) entab.c

halt.o	: halt.c 
	$(CC) -c $(XFLAGS) $(CFLAGS) halt.c

clean:
	rm -f *.o $(TARGET) log core

install:
	make clean
	make CFLAGS=-O
	install -s -o bin -g bin $(TARGET) $(BINDIR)
	rm -f $(BINDIR)/detab
	ln /usr/local/bin/$(TARGET) $(BINDIR)/detab

