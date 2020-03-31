#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/libpq/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PGFILEDESC = "PostgreSQL Access Library"

# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 5
SO_MINOR_VERSION= $(MAJORVERSION)

override CPPFLAGS :=  -DFRONTEND -DUNSAFE_STAT_OK -I$(srcdir) $(CPPFLAGS) -I$(top_builddir)/src/port -I$(top_srcdir)/src/port
ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

# The MSVC build system scrapes OBJS from this file.  If you change any of
# the conditional additions of files to OBJS, update Mkvcbuild.pm to match.

OBJS = \
	$(WIN32RES) \
	fe-auth-scram.o \
	fe-connect.o \
	fe-exec.o \
	fe-lobj.o \
	fe-misc.o \
	fe-print.o \
	fe-protocol2.o \
	fe-protocol3.o \
	fe-secure.o \
	legacy-pqsignal.o \
	libpq-events.o \
	pqexpbuffer.o \
	fe-auth.o

ifeq ($(with_openssl),yes)
OBJS += \
	fe-secure-common.o \
	fe-secure-openssl.o
endif

ifeq ($(with_gssapi),yes)
OBJS += \
	fe-gssapi-common.o \
	fe-secure-gssapi.o
endif

ifeq ($(PORTNAME), cygwin)
override shlib = cyg$(NAME)$(DLSUFFIX)
endif

ifeq ($(PORTNAME), win32)
OBJS += \
	win32.o

ifeq ($(enable_thread_safety), yes)
OBJS += pthread-win32.o
endif
endif


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)  Note that we filter out -lpgcommon and -lpgport from LIBS and
# instead link with -lpgcommon_shlib and -lpgport_shlib, to get object files
# that are built correctly for use in a shlib.
SHLIB_LINK_INTERNAL = -lpgcommon_shlib -lpgport_shlib
ifneq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi_krb5 -lgss -lgssapi -lssl -lsocket -lnsl -lresolv -lintl -lm, $(LIBS)) $(LDAP_LIBS_FE) $(PTHREAD_LIBS)
else
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi32 -lssl -lsocket -lnsl -lresolv -lintl -lm $(PTHREAD_LIBS), $(LIBS)) $(LDAP_LIBS_FE)
endif
ifeq ($(PORTNAME), win32)
SHLIB_LINK += -lshell32 -lws2_32 -lsecur32 $(filter -leay32 -lssleay32 -lcomerr32 -lkrb5_32, $(LIBS))
endif
SHLIB_PREREQS = submake-libpgport

SHLIB_EXPORTS = exports.txt

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend


# Make dependencies on pg_config_paths.h visible in all builds.
fe-connect.o: fe-connect.c $(top_builddir)/src/port/pg_config_paths.h
fe-misc.o: fe-misc.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq-events.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq-int.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pg_service.conf.sample '$(DESTDIR)$(datadir)/pg_service.conf.sample'

installcheck:
	$(MAKE) -C test $@

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(includedir)' '$(DESTDIR)$(includedir_internal)' '$(DESTDIR)$(datadir)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(includedir)/libpq-fe.h'
	rm -f '$(DESTDIR)$(includedir)/libpq-events.h'
	rm -f '$(DESTDIR)$(includedir_internal)/libpq-int.h'
	rm -f '$(DESTDIR)$(includedir_internal)/pqexpbuffer.h'
	rm -f '$(DESTDIR)$(datadir)/pg_service.conf.sample'

clean distclean: clean-lib
	$(MAKE) -C test $@
	rm -f $(OBJS) pthread.h
# Might be left over from a Win32 client-only build
	rm -f pg_config_paths.h

maintainer-clean: distclean
	$(MAKE) -C test $@
