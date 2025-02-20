#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/libpq/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL=src/test/modules/injection_points

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_ssl with_gssapi with_krb_srvnam

PGFILEDESC = "PostgreSQL Access Library"

# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 5
SO_MINOR_VERSION= $(MAJORVERSION)

override CPPFLAGS :=  -I$(srcdir) $(CPPFLAGS) -I$(top_builddir)/src/port -I$(top_srcdir)/src/port
ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

OBJS = \
	$(WIN32RES) \
	fe-auth-oauth.o \
	fe-auth-scram.o \
	fe-cancel.o \
	fe-connect.o \
	fe-exec.o \
	fe-lobj.o \
	fe-misc.o \
	fe-print.o \
	fe-protocol3.o \
	fe-secure.o \
	fe-trace.o \
	legacy-pqsignal.o \
	libpq-events.o \
	pqexpbuffer.o \
	fe-auth.o

# File shared across all SSL implementations supported.
ifneq ($(with_ssl),no)
OBJS += \
	fe-secure-common.o
endif

ifeq ($(with_ssl),openssl)
OBJS += \
	fe-secure-openssl.o
endif

ifeq ($(with_gssapi),yes)
OBJS += \
	fe-gssapi-common.o \
	fe-secure-gssapi.o
endif

ifeq ($(with_libcurl),yes)
OBJS += fe-auth-oauth-curl.o
endif

ifeq ($(PORTNAME), cygwin)
override shlib = cyg$(NAME)$(DLSUFFIX)
endif

ifeq ($(PORTNAME), win32)
OBJS += \
	pthread-win32.o \
	win32.o
endif


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)  Note that we filter out -lpgcommon and -lpgport from LIBS and
# instead link with -lpgcommon_shlib and -lpgport_shlib, to get object files
# that are built correctly for use in a shlib.
SHLIB_LINK_INTERNAL = -lpgcommon_shlib -lpgport_shlib
ifneq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi_krb5 -lgss -lgssapi -lssl -lcurl -lsocket -lnsl -lresolv -lintl -lm, $(LIBS)) $(LDAP_LIBS_FE) $(PTHREAD_LIBS)
else
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi32 -lssl -lsocket -lnsl -lresolv -lintl -lm $(PTHREAD_LIBS), $(LIBS)) $(LDAP_LIBS_FE)
endif
ifeq ($(PORTNAME), win32)
SHLIB_LINK += -lshell32 -lws2_32 -lsecur32 $(filter -lcomerr32 -lkrb5_32, $(LIBS))
endif
SHLIB_PREREQS = submake-libpgport

SHLIB_EXPORTS = exports.txt

ifeq ($(with_ssl),openssl)
PKG_CONFIG_REQUIRES_PRIVATE = libssl, libcrypto
endif

all: all-lib libpq-refs-stamp

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend

# Check for functions that libpq must not call, currently just exit().
# (Ideally we'd reject abort() too, but there are various scenarios where
# build toolchains insert abort() calls, e.g. to implement assert().)
# If nm doesn't exist or doesn't work on shlibs, this test will do nothing,
# which is fine.  The exclusion of __cxa_atexit is necessary on OpenBSD,
# which seems to insert references to that even in pure C code. Excluding
# __tsan_func_exit is necessary when using ThreadSanitizer data race detector
# which use this function for instrumentation of function exit.
# libcurl registers an exit handler in the memory debugging code when running
# with LeakSanitizer.
# Skip the test when profiling, as gcc may insert exit() calls for that.
# Also skip the test on platforms where libpq infrastructure may be provided
# by statically-linked libraries, as we can't expect them to honor this
# coding rule.
libpq-refs-stamp: $(shlib)
ifneq ($(enable_coverage), yes)
ifeq (,$(filter solaris,$(PORTNAME)))
	@if nm -A -u $< 2>/dev/null | grep -v -e __cxa_atexit -e __tsan_func_exit -e _atexit | grep exit; then \
		echo 'libpq must not be calling any function which invokes exit'; exit 1; \
	fi
endif
endif
	touch $@

# Make dependencies on pg_config_paths.h visible in all builds.
fe-connect.o: fe-connect.c $(top_builddir)/src/port/pg_config_paths.h
fe-misc.o: fe-misc.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq-events.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq-int.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/fe-auth-sasl.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pg_service.conf.sample '$(DESTDIR)$(datadir)/pg_service.conf.sample'

test-build:
	$(MAKE) -C test all

check installcheck: export PATH := $(CURDIR)/test:$(PATH)

check: test-build all
	$(prove_check)

installcheck: test-build all
	$(prove_installcheck)

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(includedir)' '$(DESTDIR)$(includedir_internal)' '$(DESTDIR)$(datadir)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(includedir)/libpq-fe.h'
	rm -f '$(DESTDIR)$(includedir)/libpq-events.h'
	rm -f '$(DESTDIR)$(includedir_internal)/libpq-int.h'
	rm -f '$(DESTDIR)$(includedir_internal)/fe-auth-sasl.h'
	rm -f '$(DESTDIR)$(includedir_internal)/pqexpbuffer.h'
	rm -f '$(DESTDIR)$(datadir)/pg_service.conf.sample'

clean distclean: clean-lib
	$(MAKE) -C test $@
	rm -rf tmp_check
	rm -f $(OBJS) pthread.h libpq-refs-stamp
# Might be left over from a Win32 client-only build
	rm -f pg_config_paths.h
