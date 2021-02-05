#-------------------------------------------------------------------------
#
# Makefile for src/fe_utils
#
# This makefile generates a static library, libpgfeutils.a,
# for use by client applications
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# IDENTIFICATION
#    src/fe_utils/Makefile
#
#-------------------------------------------------------------------------

subdir = src/fe_utils
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = \
	archive.o \
	cancel.o \
	conditional.o \
	connect_utils.o \
	mbprint.o \
	option_utils.o \
	parallel_slot.o \
	print.o \
	psqlscan.o \
	query_utils.o \
	recovery_gen.o \
	simple_list.o \
	string_utils.o

all: libpgfeutils.a

libpgfeutils.a: $(OBJS)
	rm -f $@
	$(AR) $(AROPT) $@ $^

psqlscan.c: FLEXFLAGS = -Cfe -p -p
psqlscan.c: FLEX_NO_BACKUP=yes
psqlscan.c: FLEX_FIX_WARNING=yes

distprep: psqlscan.c

# libpgfeutils could be useful to contrib, so install it
install: all installdirs
	$(INSTALL_STLIB) libpgfeutils.a '$(DESTDIR)$(libdir)/libpgfeutils.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgfeutils.a'

clean distclean:
	rm -f libpgfeutils.a $(OBJS) lex.backup

# psqlscan.c is supposed to be in the distribution tarball,
# so do not clean it in the clean/distclean rules
maintainer-clean: distclean
	rm -f psqlscan.c
