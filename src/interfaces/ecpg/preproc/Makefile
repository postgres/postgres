#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/ecpg/preproc
#
# Copyright (c) 1998-2009, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/Makefile,v 1.142 2009/03/07 00:13:58 alvherre Exp $
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

override CFLAGS += $(PTHREAD_CFLAGS) -DECPG_COMPILE

OBJS=	preproc.o type.o ecpg.o output.o parser.o \
	keywords.o c_keywords.o ecpg_keywords.o kwlookup.o ../ecpglib/typename.o descriptor.o variable.o \
	$(WIN32RES)

all: submake-libpgport ecpg

ecpg: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) $(PTHREAD_LIBS) -o $@$(X)

# pgc is compiled as part of preproc
preproc.o: $(srcdir)/pgc.c

$(srcdir)/preproc.h: $(srcdir)/preproc.c ;

$(srcdir)/preproc.c: $(srcdir)/preproc.y
ifdef BISON
	$(BISON) -d $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

$(srcdir)/pgc.c: $(srcdir)/pgc.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

$(srcdir)/preproc.y: $(top_srcdir)/src/backend/parser/gram.y parse.pl ecpg.addons ecpg.header ecpg.tokens ecpg.trailer ecpg.type
	$(PERL) $(srcdir)/parse.pl $(srcdir) < $< > $@ 

ecpg_keywords.o c_keywords.o keywords.o preproc.o parser.o: preproc.h

kwlookup.c: % : $(top_srcdir)/src/backend/parser/%
	rm -f $@ && $(LN_S) $< .

distprep: $(srcdir)/preproc.y $(srcdir)/preproc.c $(srcdir)/preproc.h $(srcdir)/pgc.c

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) '$(DESTDIR)$(bindir)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ecpg$(X)'

clean distclean:
	rm -f kwlookup.c *.o ecpg$(X)
# garbage from development
	@rm -f core a.out

# `make distclean' must not remove preproc.y, preproc.c, preproc.h, or pgc.c
# since we want to ship those files in the distribution for people with
# inadequate tools.  Instead, `make maintainer-clean' will remove them.
maintainer-clean: distclean
	rm -f $(srcdir)/preproc.y $(srcdir)/preproc.c $(srcdir)/preproc.h $(srcdir)/pgc.c
