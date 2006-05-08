#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files, and for common code sharing.
#
# This makefile generates two outputs:
#
#	libpgport.a - contains object files with FRONTEND defined,
#		for use by client application and libraries
#
#	libpgport_srv.a - contains object files without FRONTEND defined,
#		for use only by the backend binaries
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/port/Makefile,v 1.29.2.1 2006/05/08 02:18:59 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

# Replace all object files so they use FRONTEND define
LIBOBJS_SRV := $(LIBOBJS:%.o=%_srv.o)

all: libpgport.a libpgport_srv.a

# libpgport is needed by some contrib
install: all
	$(INSTALL_STLIB) libpgport.a $(DESTDIR)$(libdir)

uninstall:
	$(RM) $(DESTDIR)$(libdir)/libpgport.a

libpgport.a: $(LIBOBJS)
	$(AR) $(AROPT) $@ $^

thread.o: thread.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PTHREAD_CFLAGS) -c $<

path.o: path.c pg_config_paths.h

#
# Server versions of object files
#

libpgport_srv.a: $(LIBOBJS_SRV)
	$(AR) $(AROPT) $@ $^

%_srv.o: %.c
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

# No thread flags for server version
thread_srv.o: thread.c
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

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
	echo "#define DOCDIR \"$(docdir)\"" >>$@
	echo "#define MANDIR \"$(mandir)\"" >>$@

clean distclean maintainer-clean:
	rm -f libpgport.a libpgport_srv.a $(LIBOBJS) $(LIBOBJS_SRV) pg_config_paths.h
