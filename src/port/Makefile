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
#    $PostgreSQL: pgsql/src/port/Makefile,v 1.20 2004/10/06 08:50:02 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_builddir)/src/port -DFRONTEND $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

# Replace object files that use FRONTEND define
LIBOBJS_SRV := $(LIBOBJS)
LIBOBJS_SRV := $(patsubst dirmod.o,dirmod_srv.o, $(LIBOBJS_SRV))
LIBOBJS_SRV := $(patsubst exec.o,exec_srv.o, $(LIBOBJS_SRV))
LIBOBJS_SRV := $(patsubst getaddrinfo.o,getaddrinfo_srv.o, $(LIBOBJS_SRV))
LIBOBJS_SRV := $(patsubst thread.o,thread_srv.o, $(LIBOBJS_SRV))

all: libpgport.a libpgport_srv.a

# libpgport is needed by some contrib
install:
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

dirmod_srv.o: dirmod.c
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

exec_srv.o: exec.c
	$(CC) $(CFLAGS) $(subst -DFRONTEND,, $(CPPFLAGS)) -c $< -o $@

getaddrinfo_srv.o: getaddrinfo.c
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

clean distclean maintainer-clean:
	rm -f libpgport.a libpgport_srv.a $(LIBOBJS) $(LIBOBJS_SRV) pg_config_paths.h
