#
# Makefile
#
#
TARGET = entab
BINDIR = /usr/local/bin
XFLAGS =
CFLAGS = -O $(XFLAGS)
LIBS =

$(TARGET): entab.o
	$(CC) -o $@ $(CFLAGS) $^ $(LIBS)

clean:
	rm -f *.o $(TARGET) log core

install: $(TARGET)
	install -s $(TARGET) $(BINDIR)
	rm -f $(BINDIR)/detab
	ln $(BINDIR)/$(TARGET) $(BINDIR)/detab
