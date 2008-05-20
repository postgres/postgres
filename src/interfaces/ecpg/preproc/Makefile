#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/ecpg/preproc
#
# Copyright (c) 1998-2008, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/Makefile,v 1.133 2008/05/20 23:17:32 meskes Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "ecpg - embedded SQL precompiler for C"
subdir = src/interfaces/ecpg/preproc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

MAJOR_VERSION= 4
MINOR_VERSION= 5
PATCHLEVEL=0

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(srcdir) -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	 $(CPPFLAGS)

override CFLAGS += $(PTHREAD_CFLAGS)

OBJS=	preproc.o type.o ecpg.o output.o parser.o \
	keywords.o c_keywords.o ecpg_keywords.o ../ecpglib/typename.o descriptor.o variable.o \
	$(WIN32RES)

all: submake-libpgport ecpg

ecpg: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) $(PTHREAD_LIBS) -o $@$(X)

# pgc is compiled as part of preproc
preproc.o: $(srcdir)/pgc.c

$(srcdir)/preproc.c: $(srcdir)/preproc.h ;

$(srcdir)/preproc.h: preproc.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c $(srcdir)/preproc.c
	mv -f y.tab.h $(srcdir)/preproc.h
else
	@$(missing) bison $< $@
endif

$(srcdir)/pgc.c: pgc.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

c_keywords.o keywords.o preproc.o parser.o: preproc.h

# instead of maintaining our own list, take the one from the backend
# we cannot just link it in, but must copy and make some minor changes
keywords.c: % : $(top_srcdir)/src/backend/parser/%
	sed -e 's/#include "parser\/parse.h"/#include "preproc.h"/' $< > $@

distprep: $(srcdir)/preproc.c $(srcdir)/preproc.h $(srcdir)/pgc.c

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) '$(DESTDIR)$(bindir)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ecpg$(X)'

clean distclean:
	rm -f keywords.c *.o ecpg$(X)
# garbage from partial builds
	@rm -f y.tab.c y.tab.h
# garbage from development
	@rm -f core a.out *.output *.tab.c

# `make clean' does not remove preproc.c, preproc.h, or pgc.c since we
# want to ship those files in the distribution for people with
# inadequate tools.
maintainer-clean: distclean
	rm -f $(srcdir)/preproc.c $(srcdir)/preproc.h $(srcdir)/pgc.c
