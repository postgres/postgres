#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/ecpg/preproc
#
# Copyright (c) 1998-2017, PostgreSQL Global Development Group
#
# src/interfaces/ecpg/preproc/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "ecpg - embedded SQL precompiler for C"
PGAPPICON=win32

subdir = src/interfaces/ecpg/preproc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I. -I$(srcdir) -DECPG_COMPILE \
	 $(CPPFLAGS)

override CFLAGS += $(PTHREAD_CFLAGS)

OBJS=	preproc.o pgc.o type.o ecpg.o output.o parser.o \
	keywords.o c_keywords.o ecpg_keywords.o ../ecpglib/typename.o descriptor.o variable.o \
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

preproc.h: preproc.c ;
preproc.c: BISONFLAGS += -d

preproc.y: ../../../backend/parser/gram.y parse.pl ecpg.addons ecpg.header ecpg.tokens ecpg.trailer ecpg.type
	$(PERL) $(srcdir)/parse.pl $(srcdir) < $< > $@
	$(PERL) $(srcdir)/check_rules.pl $(srcdir) $<

ecpg_keywords.o c_keywords.o keywords.o preproc.o pgc.o parser.o: preproc.h

distprep: preproc.y preproc.c preproc.h pgc.c

install: all installdirs
	$(INSTALL_PROGRAM) ecpg$(X) '$(DESTDIR)$(bindir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/ecpg$(X)'

clean distclean:
	rm -f *.o ecpg$(X)
# garbage from development
	@rm -f core a.out

# `make distclean' must not remove preproc.y, preproc.c, preproc.h, or pgc.c
# since we want to ship those files in the distribution for people with
# inadequate tools.  Instead, `make maintainer-clean' will remove them.
maintainer-clean: distclean
	rm -f preproc.y preproc.c preproc.h pgc.c
