#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/interfaces/libpq/Makefile
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 5
SO_MINOR_VERSION= 8

override CPPFLAGS :=  -DFRONTEND -DUNSAFE_STAT_OK -I$(srcdir) $(CPPFLAGS) -I$(top_builddir)/src/port -I$(top_srcdir)/src/port
ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

# Need to recompile any external C files because we need
# all object files to use the same compile flags as libpq; some
# platforms require special flags.
LIBS := $(LIBS:-lpgport=)

# We can't use Makefile variables here because the MSVC build system scrapes
# OBJS from this file.
OBJS=	fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-print.o fe-lobj.o \
	fe-protocol2.o fe-protocol3.o pqexpbuffer.o fe-secure.o \
	libpq-events.o
# libpgport C files we always use
OBJS += chklocale.o inet_net_ntop.o noblock.o pgstrcasecmp.o pqsignal.o \
	thread.o
# libpgport C files that are needed if identified by configure
OBJS += $(filter crypt.o getaddrinfo.o getpeereid.o inet_aton.o open.o system.o snprintf.o strerror.o strlcpy.o win32error.o win32setlocale.o, $(LIBOBJS))
# backend/libpq
OBJS += ip.o md5.o
# utils/mb
OBJS += encnames.o wchar.o

ifeq ($(with_openssl),yes)
OBJS += fe-secure-openssl.o
endif

ifeq ($(PORTNAME), cygwin)
override shlib = cyg$(NAME)$(DLSUFFIX)
endif

ifeq ($(PORTNAME), win32)
# pgsleep.o is from libpgport
OBJS += pgsleep.o win32.o libpqrc.o

libpqrc.o: libpq.rc
	$(WINDRES) -i $< -o $@

ifeq ($(enable_thread_safety), yes)
OBJS += pthread-win32.o
endif
endif


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)
ifneq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi_krb5 -lgss -lgssapi -lssl -lsocket -lnsl -lresolv -lintl, $(LIBS)) $(LDAP_LIBS_FE) $(PTHREAD_LIBS)
else
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi32 -lssl -lsocket -lnsl -lresolv -lintl $(PTHREAD_LIBS), $(LIBS)) $(LDAP_LIBS_FE)
endif
ifeq ($(PORTNAME), win32)
SHLIB_LINK += -lshell32 -lws2_32 -lsecur32 $(filter -leay32 -lssleay32 -lcomerr32 -lkrb5_32, $(LIBS))
endif

SHLIB_EXPORTS = exports.txt

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend


# We use several libpgport and backend modules verbatim, but since we need
# to compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files built for libpgport and the backend.
# Instead, symlink the source files in here and build our own object files.
# For some libpgport modules, this only happens if configure decides
# the module is needed (see filter hack in OBJS, above).

chklocale.c crypt.c getaddrinfo.c getpeereid.c inet_aton.c inet_net_ntop.c noblock.c open.c system.c pgsleep.c pgstrcasecmp.c pqsignal.c snprintf.c strerror.c strlcpy.c thread.c win32error.c win32setlocale.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

ip.c md5.c: % : $(backend_src)/libpq/%
	rm -f $@ && $(LN_S) $< .

encnames.c wchar.c: % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .


distprep: libpq-dist.rc

libpq.rc libpq-dist.rc: libpq.rc.in
	sed -e 's/\(VERSION.*\),0 *$$/\1,'`date '+%y%j' | sed 's/^0*//'`'/' $< >$@

# Depend on Makefile.global to force rebuild on re-run of configure.
# (But libpq-dist.rc is shipped in the distribution for shell-less
# installations and is only updated by distprep.)
libpq.rc: $(top_builddir)/src/Makefile.global

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
ifneq (,$(findstring $(PORTNAME), win32 cygwin))
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(bindir)/$(shlib)'
endif

installcheck:
	$(MAKE) -C test $@

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(includedir)' '$(DESTDIR)$(includedir_internal)' '$(DESTDIR)$(datadir)'
ifneq (,$(findstring $(PORTNAME), win32 cygwin))
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
endif

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(includedir)/libpq-fe.h'
	rm -f '$(DESTDIR)$(includedir)/libpq-events.h'
	rm -f '$(DESTDIR)$(includedir_internal)/libpq-int.h'
	rm -f '$(DESTDIR)$(includedir_internal)/pqexpbuffer.h'
	rm -f '$(DESTDIR)$(datadir)/pg_service.conf.sample'
ifneq (,$(findstring $(PORTNAME), win32 cygwin))
	rm -f '$(DESTDIR)$(bindir)/$(shlib)'
endif

clean distclean: clean-lib
	$(MAKE) -C test $@
	rm -f $(OBJS) pthread.h libpq.rc
# Might be left over from a Win32 client-only build
	rm -f pg_config_paths.h
	rm -f inet_net_ntop.c noblock.c pgstrcasecmp.c pqsignal.c thread.c
	rm -f chklocale.c crypt.c getaddrinfo.c getpeereid.c inet_aton.c open.c system.c snprintf.c strerror.c strlcpy.c win32error.c win32setlocale.c
	rm -f pgsleep.c
	rm -f md5.c ip.c
	rm -f encnames.c wchar.c

maintainer-clean: distclean maintainer-clean-lib
	$(MAKE) -C test $@
	rm -f libpq-dist.rc
