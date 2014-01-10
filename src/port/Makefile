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

OBJS = $(LIBOBJS) chklocale.o dirmod.o erand48.o fls.o inet_net_ntop.o \
	noblock.o path.o pgcheckdir.o pg_crc.o pgmkdirp.o pgsleep.o \
	pgstrcasecmp.o pqsignal.o \
	qsort.o qsort_arg.o quotes.o sprompt.o tar.o thread.o

# foo_srv.o and foo.o are both built from foo.c, but only foo.o has -DFRONTEND
OBJS_SRV = $(OBJS:%.o=%_srv.o)

all: libpgport.a libpgport_srv.a

# libpgport is needed by some contrib
install: all installdirs
	$(INSTALL_STLIB) libpgport.a '$(DESTDIR)$(libdir)/libpgport.a'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'

uninstall:
	rm -f '$(DESTDIR)$(libdir)/libpgport.a'

libpgport.a: $(OBJS)
	$(AR) $(AROPT) $@ $^

# thread.o needs PTHREAD_CFLAGS (but thread_srv.o does not)
thread.o: CFLAGS+=$(PTHREAD_CFLAGS)

#
# Server versions of object files
#

libpgport_srv.a: $(OBJS_SRV)
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

$(OBJS_SRV): | submake-errcodes

.PHONY: submake-errcodes

submake-errcodes:
	$(MAKE) -C ../backend submake-errcodes

# Dependency is to ensure that path changes propagate

path.o: path.c pg_config_paths.h

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

clean distclean maintainer-clean:
	rm -f libpgport.a libpgport_srv.a $(OBJS) $(OBJS_SRV) pg_config_paths.h
