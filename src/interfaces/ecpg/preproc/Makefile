subdir = src/interfaces/ecpg/preproc
top_builddir = ../../../..
include ../../../Makefile.global

MAJOR_VERSION=2
MINOR_VERSION=7
PATCHLEVEL=1

CFLAGS+=-I../include -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	-DINCLUDE_PATH=\"$(includedir)\" 

OBJS=preproc.o pgc.o type.o ecpg.o ecpg_keywords.o output.o\
    keywords.o c_keywords.o ../lib/typename.o descriptor.o variable.o

all: ecpg

ifdef SNPRINTF
OBJS+=$(top_builddir)/src/backend/port/snprintf.o

$(top_builddir)/src/backend/port/snprintf.o:
	$(MAKE) -C $(top_builddir)/src/backend/port snprintf.o
endif


ecpg: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(srcdir)/preproc.c $(srcdir)/preproc.h: preproc.y
	$(YACC) -d $(YFLAGS) $<
	mv y.tab.c $(srcdir)/preproc.c
	mv y.tab.h $(srcdir)/preproc.h

$(srcdir)/pgc.c: pgc.l
	$(LEX) $(LFLAGS) $<
	mv lex.yy.c $@

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) $(bindir)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/ecpg$(X)

clean distclean:
	rm -f *.o ecpg$(X)
# garbage from partial builds
	rm -f y.tab.c y.tab.h lex.yy.c
# garbage from development
	@rm -f core a.out *~ *.output *.tab.c

# `make clean' does not remove preproc.c, preproc.h, or pgc.c since we
# want to ship those files in the distribution for people with
# inadequate tools.
maintainer-clean: distclean
	rm -f $(srcdir)/preproc.c $(srcdir)/preproc.h $(srcdir)/pgc.c


depend dep: preproc.c pgc.c
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
