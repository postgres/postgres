#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_basebackup
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/pg_basebackup/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_basebackup/pg_receivewal/pg_recvlogical - streaming WAL and backup receivers"
PGAPPICON=win32

EXTRA_INSTALL=contrib/test_decoding

subdir = src/bin/pg_basebackup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# make these available to TAP test scripts
export LZ4
export TAR
# Note that GZIP cannot be used directly as this environment variable is
# used by the command "gzip" to pass down options, so stick with a different
# name.
export GZIP_PROGRAM=$(GZIP)

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

OBJS = \
	$(WIN32RES) \
	receivelog.o \
	streamutil.o \
	walmethods.o

# If you add or remove files here, also update Mkvcbuild.pm, which only knows
# about OBJS, not BBOBJS, and thus has to be manually updated to stay in sync
# with this list.
BBOBJS = \
	pg_basebackup.o \
	bbstreamer_file.o \
	bbstreamer_gzip.o \
	bbstreamer_inject.o \
	bbstreamer_lz4.o \
	bbstreamer_zstd.o \
	bbstreamer_tar.o

all: pg_basebackup pg_receivewal pg_recvlogical

pg_basebackup: $(BBOBJS) $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $(BBOBJS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

pg_receivewal: pg_receivewal.o $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) pg_receivewal.o $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

pg_recvlogical: pg_recvlogical.o $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) pg_recvlogical.o $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) pg_basebackup$(X) '$(DESTDIR)$(bindir)/pg_basebackup$(X)'
	$(INSTALL_PROGRAM) pg_receivewal$(X) '$(DESTDIR)$(bindir)/pg_receivewal$(X)'
	$(INSTALL_PROGRAM) pg_recvlogical$(X) '$(DESTDIR)$(bindir)/pg_recvlogical$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_basebackup$(X)'
	rm -f '$(DESTDIR)$(bindir)/pg_receivewal$(X)'
	rm -f '$(DESTDIR)$(bindir)/pg_recvlogical$(X)'

clean distclean maintainer-clean:
	rm -f pg_basebackup$(X) pg_receivewal$(X) pg_recvlogical$(X) \
		$(BBOBJS) pg_receivewal.o pg_recvlogical.o \
		$(OBJS)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
