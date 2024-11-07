#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/port
#
# These files are used by the Postgres backend, and also by frontend
# programs.  Primarily, they are meant to provide portability on systems
# with broken/missing library files.
#
# This makefile generates three outputs:
#
#	libpgport.a - contains object files with FRONTEND defined,
#		for use by client applications
#
#	libpgport_shlib.a - contains object files with FRONTEND defined,
#		built suitably for use in shared libraries; for use
#		by frontend libraries
#
#	libpgport_srv.a - contains object files without FRONTEND defined,
#		for use only by the backend
#
# LIBOBJS is set by configure (via Makefile.global) to be the list of object
# files that are conditionally needed as determined by configure's probing.
# OBJS adds additional object files that are always compiled.
#
# IDENTIFICATION
#    src/port/Makefile
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

OBJS = \
	$(LIBOBJS) \
	$(PG_CRC32C_OBJS) \
	bsearch_arg.o \
	chklocale.o \
	inet_net_ntop.o \
	noblock.o \
	path.o \
	pg_bitutils.o \
	pg_popcount_avx512.o \
	pg_strong_random.o \
	pgcheckdir.o \
	pgmkdirp.o \
	pgsleep.o \
	pgstrcasecmp.o \
	pgstrsignal.o \
	pqsignal.o \
	qsort.o \
	qsort_arg.o \
	quotes.o \
	snprintf.o \
	strerror.o \
	tar.o

# libpgport.a, libpgport_shlib.a, and libpgport_srv.a contain the same files
# foo.o, foo_shlib.o, and foo_srv.o are all built from foo.c
OBJS_SHLIB = $(OBJS:%.o=%_shlib.o)
OBJS_SRV = $(OBJS:%.o=%_srv.o)

all: libpgport.a libpgport_shlib.a libpgport_srv.a

# libpgport is needed by some contrib
install: all installdirs
	$(INSTALL_STLIB) libpgport.a '$(DESTDIR)$(libdir)/libpgport.a'
	$(INSTALL_STLIB) libpgport_shlib.a '$(DESTDIR)$(libdir)/libpgport_shlib.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgport.a'
	rm -f '$(DESTDIR)$(libdir)/libpgport_shlib.a'

libpgport.a: $(OBJS)
	rm -f $@
	$(AR) $(AROPT) $@ $^

# all versions of pg_crc32c_sse42.o need CFLAGS_CRC
pg_crc32c_sse42.o: CFLAGS+=$(CFLAGS_CRC)
pg_crc32c_sse42_shlib.o: CFLAGS+=$(CFLAGS_CRC)
pg_crc32c_sse42_srv.o: CFLAGS+=$(CFLAGS_CRC)

# all versions of pg_crc32c_armv8.o need CFLAGS_CRC
pg_crc32c_armv8.o: CFLAGS+=$(CFLAGS_CRC)
pg_crc32c_armv8_shlib.o: CFLAGS+=$(CFLAGS_CRC)
pg_crc32c_armv8_srv.o: CFLAGS+=$(CFLAGS_CRC)

#
# Shared library versions of object files
#

libpgport_shlib.a: $(OBJS_SHLIB)
	rm -f $@
	$(AR) $(AROPT) $@ $^

# Because this uses its own compilation rule, it doesn't use the
# dependency tracking logic from Makefile.global.  To make sure that
# dependency tracking works anyway for the *_shlib.o files, depend on
# their *.o siblings as well, which do have proper dependencies.  It's
# a hack that might fail someday if there is a *_shlib.o without a
# corresponding *.o, but there seems little reason for that.
%_shlib.o: %.c %.o
	$(CC) $(CFLAGS) $(CFLAGS_SL) $(CPPFLAGS) -c $< -o $@

#
# Server versions of object files
#

libpgport_srv.a: $(OBJS_SRV)
	rm -f $@
	$(AR) $(AROPT) $@ $^

# Because this uses its own compilation rule, it doesn't use the
# dependency tracking logic from Makefile.global.  To make sure that
# dependency tracking works anyway for the *_srv.o files, depend on
# their *.o siblings as well, which do have proper dependencies.  It's
# a hack that might fail someday if there is a *_srv.o without a
# corresponding *.o, but it works for now (and those would probably go
# into src/backend/port/ anyway).
%_srv.o: %.c %.o
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

# Dependency is to ensure that path changes propagate

path.o: path.c pg_config_paths.h

path_shlib.o: path.c pg_config_paths.h

path_srv.o: path.c pg_config_paths.h

# We create a separate file rather than put these in pg_config.h
# because many of these values come from makefiles and are not
# available to configure.
pg_config_paths.h: $(top_builddir)/src/Makefile.global
	echo "#define PGBINDIR \"$(bindir)\"" >$@
	echo "#define PGSHAREDIR \"$(datadir)\"" >>$@
	echo "#define SYSCONFDIR \"$(sysconfdir)\"" >>$@
	echo "#define INCLUDEDIR \"$(includedir)\"" >>$@
	echo "#define PKGINCLUDEDIR \"$(pkgincludedir)\"" >>$@
	echo "#define INCLUDEDIRSERVER \"$(includedir_server)\"" >>$@
	echo "#define LIBDIR \"$(libdir)\"" >>$@
	echo "#define PKGLIBDIR \"$(pkglibdir)\"" >>$@
	echo "#define LOCALEDIR \"$(localedir)\"" >>$@
	echo "#define DOCDIR \"$(docdir)\"" >>$@
	echo "#define HTMLDIR \"$(htmldir)\"" >>$@
	echo "#define MANDIR \"$(mandir)\"" >>$@

clean distclean:
	rm -f libpgport.a libpgport_shlib.a libpgport_srv.a
	rm -f $(OBJS) $(OBJS_SHLIB) $(OBJS_SRV) pg_config_paths.h
