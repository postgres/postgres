SRCDIR= ../../..
include $(SRCDIR)/Makefile.global

MAJOR_VERSION=2
MINOR_VERSION=7
PATCHLEVEL=14

CFLAGS+=-I../include -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	-DINCLUDE_PATH=\"$(HEADERDIR)\" -g

OBJ=preproc.o pgc.o type.o ecpg.o ecpg_keywords.o \
    keywords.o c_keywords.o ../lib/typename.o
#../../../backend/parser/scansup.o

all:: ecpg

# Rule that really do something.
ecpg: $(OBJ)
	$(CC) -o ecpg $(OBJ) $(LEXLIB) $(LDFLAGS)

preproc.c preproc.h: preproc.y
	$(YACC) $(YFLAGS) $<
	mv y.tab.c preproc.c
	mv y.tab.h preproc.h

pgc.c: pgc.l
	$(LEX) $<
	mv lex.yy.c pgc.c

clean:
	rm -f *.o core a.out ecpg$(X) *~ *.output
# And the garbage that might have been left behind by partial build:
	rm -f y.tab.c y.tab.h lex.yy.c
# make clean does not remove preproc.c, preproc.h, pgc.c since we want
# to ship those files in the distribution, for people with inadequate tools.

install: all
	$(INSTALL) $(INSTL_EXE_OPTS) ecpg$(X) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/ecpg

preproc.o: preproc.h ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c
type.o: ../include/ecpgtype.h
pgc.o: ../include/ecpgtype.h keywords.c c_keywords.c ecpg_keywords.c preproc.h
keywords.o: ../include/ecpgtype.h preproc.h
c_keywords.o: ../include/ecpgtype.h preproc.h 
ecpg_keywords.o: ../include/ecpgtype.h preproc.h 

