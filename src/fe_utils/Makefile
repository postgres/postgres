#-------------------------------------------------------------------------
#
# Makefile for src/fe_utils
#
# This makefile generates a static library, libpgfeutils.a,
# for use by client applications
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
	astreamer_file.o \
	astreamer_gzip.o \
	astreamer_lz4.o \
	astreamer_tar.o \
	astreamer_zstd.o \
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

ifeq ($(PORTNAME), win32)
override CPPFLAGS += -DFD_SETSIZE=1024
endif

all: libpgfeutils.a

libpgfeutils.a: $(OBJS)
	rm -f $@
	$(AR) $(AROPT) $@ $^

psqlscan.c: FLEXFLAGS = -Cfe -p -p
psqlscan.c: FLEX_NO_BACKUP=yes

# libpgfeutils could be useful to contrib, so install it
install: all installdirs
	$(INSTALL_STLIB) libpgfeutils.a '$(DESTDIR)$(libdir)/libpgfeutils.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgfeutils.a'

clean distclean:
	rm -f libpgfeutils.a $(OBJS) lex.backup
	rm -f psqlscan.c
