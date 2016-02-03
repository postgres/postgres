#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/ecpg/preproc
#
# Copyright (c) 1998-2016, PostgreSQL Global Development Group
#
# src/interfaces/ecpg/preproc/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "ecpg - embedded SQL precompiler for C"
PGAPPICON=win32

subdir = src/interfaces/ecpg/preproc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

MAJOR_VERSION= 4
MINOR_VERSION= 12
PATCHLEVEL=0

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I. -I$(srcdir) -DMAJOR_VERSION=$(MAJOR_VERSION) \
	-DMINOR_VERSION=$(MINOR_VERSION) -DPATCHLEVEL=$(PATCHLEVEL) \
	 $(CPPFLAGS)

override CFLAGS += $(PTHREAD_CFLAGS) -DECPG_COMPILE

OBJS=	preproc.o type.o ecpg.o output.o parser.o \
	keywords.o c_keywords.o ecpg_keywords.o kwlookup.o ../ecpglib/typename.o descriptor.o variable.o \
	$(WIN32RES)

# Suppress parallel build to avoid a bug in GNU make 3.82
# (see comments in ../Makefile)
ifeq ($(MAKE_VERSION),3.82)
.NOTPARALLEL:
endif

all: ecpg

ecpg: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) $^ $(LIBS) $(PTHREAD_LIBS) -o $@$(X)

../ecpglib/typename.o: ../ecpglib/typename.c
	$(MAKE) -C $(dir $@) $(notdir $@)

# pgc is compiled as part of preproc
preproc.o: pgc.c

preproc.h: preproc.c ;
preproc.c: BISONFLAGS += -d

preproc.y: ../../../backend/parser/gram.y parse.pl ecpg.addons ecpg.header ecpg.tokens ecpg.trailer ecpg.type
	$(PERL) $(srcdir)/parse.pl $(srcdir) < $< > $@
	$(PERL) $(srcdir)/check_rules.pl $(srcdir) $<

ecpg_keywords.o c_keywords.o keywords.o preproc.o parser.o: preproc.h

kwlookup.c: % : $(top_srcdir)/src/backend/parser/%
	rm -f $@ && $(LN_S) $< .

distprep: preproc.y preproc.c preproc.h pgc.c

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) '$(DESTDIR)$(bindir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

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
	rm -f preproc.y preproc.c preproc.h pgc.c
