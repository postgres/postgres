SRCDIR= ../../..
include $(SRCDIR)/Makefile.global

MAJOR_VERSION=1
MINOR_VERSION=1
PATCHLEVEL=0

CFLAGS+=-I../include -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	-DINCLUDE_PATH=\"$(DESTDIR)$(HEADERDIR)\"

all:: ecpg

clean:
	rm -f *.o core a.out ecpg y.tab.h y.tab.c pgc.c *~

install: all
	$(INSTALL) $(INSTL_EXE_OPTS) ecpg $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ecpg

# Rule that really do something.
ecpg: y.tab.o pgc.o type.o ecpg.o ../lib/typename.o
	$(CC) -o ecpg y.tab.o pgc.o type.o ecpg.o ../lib/typename.o $(LEXLIB)

y.tab.h y.tab.c: preproc.y
	$(YACC) $(YFLAGS) $<

y.tab.o : y.tab.h ../include/ecpgtype.h
type.o : ../include/ecpgtype.h
pgc.o : ../include/ecpgtype.h
