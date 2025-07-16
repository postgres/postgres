#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_rewind
#
# Portions Copyright (c) 2013-2024, PostgreSQL Global Development Group
#
# src/bin/pg_rewind/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_rewind - synchronize a data directory with another one forked from"
PGAPPICON = win32

subdir = src/bin/pg_rewind
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) -DFRONTEND $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	datapagemap.o \
	file_ops.o \
	filemap.o \
	libpq_source.o \
	local_source.o \
	parsexlog.o \
	pg_rewind.o \
	timeline.o \
	xlogreader.o

ifeq ($(enable_percona_ext),yes)

OBJS += \
	$(top_srcdir)/src/fe_utils/simple_list.o \
	$(top_builddir)/src/libtde/libtdexlog.a \
	$(top_builddir)/src/libtde/libtde.a

override CPPFLAGS := -I$(top_srcdir)/contrib/pg_tde/src/include -I$(top_srcdir)/contrib/pg_tde/src/libkmip/libkmip/include $(CPPFLAGS)
endif

EXTRA_CLEAN = xlogreader.c

all: pg_rewind

pg_rewind: $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_rewind$(X) '$(DESTDIR)$(bindir)/pg_rewind$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_rewind$(X)'

clean distclean:
	rm -f pg_rewind$(X) $(OBJS) xlogreader.c
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
