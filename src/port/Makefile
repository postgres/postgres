#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files.

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/port/Makefile,v 1.16 2004/08/01 06:56:39 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_builddir)/src/port $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

all: libpgport.a

libpgport.a: $(LIBOBJS)
	$(AR) $(AROPT) $@ $^

thread.o: thread.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PTHREAD_CFLAGS) -c $<

path.o: path.c pg_config_paths.h

# Dependency is to ensure that path changes propagate
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

clean distclean maintainer-clean:
	rm -f libpgport.a $(LIBOBJS) pg_config_paths.h
