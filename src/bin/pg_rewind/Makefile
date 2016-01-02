#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_rewind
#
# Portions Copyright (c) 2013-2016, PostgreSQL Global Development Group
#
# src/bin/pg_rewind/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_rewind - synchronize a data directory with another one forked from"
PGAPPICON = win32

subdir = src/bin/pg_rewind
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

override CPPFLAGS := -I$(libpq_srcdir) -DFRONTEND $(CPPFLAGS)

OBJS	= pg_rewind.o parsexlog.o xlogreader.o datapagemap.o timeline.o \
	fetch.o file_ops.o copy_fetch.o libpq_fetch.o filemap.o logging.o \
	$(WIN32RES)

EXTRA_CLEAN = xlogreader.c

all: pg_rewind

pg_rewind: $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_rewind$(X) '$(DESTDIR)$(bindir)/pg_rewind$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_rewind$(X)'

clean distclean maintainer-clean:
	rm -f pg_rewind$(X) $(OBJS) xlogreader.c
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
