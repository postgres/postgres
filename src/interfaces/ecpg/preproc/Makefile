SRCDIR= ../../..
include $(SRCDIR)/Makefile.global

MAJOR_VERSION=2
MINOR_VERSION=3
PATCHLEVEL=0

CFLAGS+=-I../include -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	-DINCLUDE_PATH=\"$(DESTDIR)$(HEADERDIR)\"

OBJ=y.tab.o pgc.o type.o ecpg.o ecpg_keywords.o ../../../backend/parser/scansup.o \
    keywords.o c_keywords.o ../lib/typename.o

all:: ecpg

clean:
	rm -f *.o core a.out ecpg y.tab.h y.tab.c pgc.c *~

install: all
	$(INSTALL) $(INSTL_EXE_OPTS) ecpg $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ecpg

# Rule that really do something.
ecpg: $(OBJ)
	$(CC) -o ecpg $(OBJ) $(LEXLIB)

y.tab.h y.tab.c: preproc.y
	$(YACC) $(YFLAGS) $<

y.tab.o : y.tab.h ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c
type.o : ../include/ecpgtype.h
pgc.o : ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c y.tab.h
keywords.o: ../include/ecpgtype.h y.tab.h
c_keywords.o: ../include/ecpgtype.h y.tab.h
ecpg_keywords.o: ../include/ecpgtype.h y.tab.h

