SRCDIR= ../../..
include $(SRCDIR)/Makefile.global

MAJOR_VERSION=3
MINOR_VERSION=0
PATCHLEVEL=0

CFLAGS+=-I../include -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	-DINCLUDE_PATH=\"$(DESTDIR)$(HEADERDIR)\"

OBJ=preproc.o pgc.o type.o ecpg.o ecpg_keywords.o ../../../backend/parser/scansup.o \
    keywords.o c_keywords.o ../lib/typename.o

all:: ecpg

preproc.c preproc.h: preproc.y
	$(YACC) $(YFLAGS) $<
	mv y.tab.c preproc.c
	mv y.tab.h preproc.h

clean:
	rm -f *.o core a.out ecpg$(X) *~

install: all
	$(INSTALL) $(INSTL_EXE_OPTS) ecpg$(X) $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ecpg

# Rule that really do something.
ecpg: $(OBJ)
	$(CC) -o ecpg $(OBJ) $(LEXLIB) $(LDFLAGS)

pgc.c: pgc.l
	$(LEX) $<
	mv lex.yy.c pgc.c

preproc.o : preproc.h ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c
type.o : ../include/ecpgtype.h
pgc.o : ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c preproc.h
keywords.o: ../include/ecpgtype.h preproc.h
c_keywords.o: ../include/ecpgtype.h preproc.h 
ecpg_keywords.o: ../include/ecpgtype.h preproc.h 

