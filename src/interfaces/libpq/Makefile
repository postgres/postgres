#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/interfaces/libpq/Makefile,v 1.159 2008/01/01 19:46:00 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 5
SO_MINOR_VERSION= 1
DLTYPE= library

override CPPFLAGS :=  -DFRONTEND -I$(srcdir) $(CPPFLAGS) -I$(top_builddir)/src/port
ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

# Need to recompile any libpgport object files because we need these
# object files to use the same compile flags as libpq.  If we used
# the object files from libpgport, this would not be true on all
# platforms.
LIBS := $(LIBS:-lpgport=)

OBJS=	fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-print.o fe-lobj.o \
	fe-protocol2.o fe-protocol3.o pqexpbuffer.o pqsignal.o fe-secure.o \
	md5.o ip.o wchar.o encnames.o noblock.o pgstrcasecmp.o thread.o \
	$(filter crypt.o getaddrinfo.o inet_aton.o open.o snprintf.o strerror.o strlcpy.o win32error.o, $(LIBOBJS))

ifeq ($(PORTNAME), cygwin)
override shlib = cyg$(NAME)$(DLSUFFIX)
endif

ifeq ($(PORTNAME), win32)
OBJS += win32.o pgsleep.o libpqrc.o
DLL_DEFFILE=libpqdll.def

libpqrc.o: libpq.rc
	windres -i libpq.rc -o libpqrc.o

ifeq ($(enable_thread_safety), yes)
OBJS += pthread-win32.o
endif
endif


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)
ifneq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi_krb5 -lssl -lsocket -lnsl -lresolv -lintl, $(LIBS)) $(LDAP_LIBS_FE) $(PTHREAD_LIBS)
else
SHLIB_LINK += $(filter -lcrypt -ldes -lcom_err -lcrypto -lk5crypto -lkrb5 -lgssapi32 -lssl -lsocket -lnsl -lresolv -lintl $(PTHREAD_LIBS), $(LIBS)) $(LDAP_LIBS_FE)
endif
ifeq ($(PORTNAME), win32)
SHLIB_LINK += -lshfolder -lwsock32 -lws2_32 -lsecur32 $(filter -leay32 -lssleay32 -lcomerr32 -lkrb5_32, $(LIBS))
endif


all: def-files $(srcdir)/libpq.rc all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend


# We use several backend modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.
# For port modules, this only happens if configure decides the module
# is needed (see filter hack in OBJS, above).

crypt.c getaddrinfo.c inet_aton.c noblock.c open.c pgstrcasecmp.c snprintf.c strerror.c strlcpy.c thread.c win32error.c pgsleep.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

md5.c ip.c: % : $(backend_src)/libpq/%
	rm -f $@ && $(LN_S) $< .

encnames.c wchar.c : % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .


# We need several not-quite-identical variants of .DEF files to build libpq
# DLLs for Windows.  These are made from the single source file exports.txt.
# Since we can't assume that Windows boxes will have sed, the .DEF files are
# always built and included in distribution tarballs.

distprep: def-files $(srcdir)/libpq.rc

.PHONY: def-files

def-files: $(srcdir)/libpqdll.def $(srcdir)/libpqddll.def $(srcdir)/blibpqdll.def

$(srcdir)/libpqdll.def: exports.txt
	echo '; DEF file for MS VC++' > $@
	echo 'LIBRARY LIBPQ' >> $@
	echo 'EXPORTS' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    \1@ \2/' < $< >> $@

$(srcdir)/libpqddll.def: exports.txt
	echo '; DEF file for MS VC++' > $@
	echo 'LIBRARY LIBPQD' >> $@
	echo 'EXPORTS' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    \1@ \2/' < $< >> $@

$(srcdir)/blibpqdll.def: exports.txt
	echo '; DEF file for Borland C++ Builder' > $@
	echo 'LIBRARY BLIBPQ' >> $@
	echo 'EXPORTS' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    _\1@ \2/' < $< >> $@
	echo '' >> $@
	echo '; Aliases for MS compatible names' >> $@
	sed -e '/^#/d' -e 's/^\(.* \)\([0-9][0-9]*\)/    \1= _\1/' < $< | sed 's/ *$$//' >> $@

# Where possible, restrict the symbols exported by the library to just the
# official list, so as to avoid unintentional ABI changes.  On recent Darwin
# this also quiets multiply-defined-symbol warnings in programs that use
# libpgport along with libpq.

ifeq ($(PORTNAME), darwin)
$(shlib): exports.list

exports.list: exports.txt
	$(AWK) '/^[^#]/ {printf "_%s\n",$$1}' $< >$@

exported_symbols_list = -exported_symbols_list exports.list
endif

ifeq ($(PORTNAME), linux)
$(shlib): exports.list

exports.list: exports.txt
	echo '{ global:' >$@
	$(AWK) '/^[^#]/ {printf "%s;\n",$$1}' $< >>$@
	echo ' local: *; };' >>$@

exported_symbols_list = -Wl,--version-script=exports.list
endif

# depend on Makefile.global to force rebuild on re-run of configure
$(srcdir)/libpq.rc: libpq.rc.in $(top_builddir)/src/Makefile.global
	sed -e 's/\(VERSION.*\),0 *$$/\1,'`date '+%y%j' | sed 's/^0*//'`'/' < $< > $@

fe-connect.o: fe-connect.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h:
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h '$(DESTDIR)$(includedir)'
	$(INSTALL_DATA) $(srcdir)/libpq-int.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h '$(DESTDIR)$(includedir_internal)'
	$(INSTALL_DATA) $(srcdir)/pg_service.conf.sample '$(DESTDIR)$(datadir)/pg_service.conf.sample'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(libdir)' '$(DESTDIR)$(includedir)' '$(DESTDIR)$(includedir_internal)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(includedir)/libpq-fe.h' '$(DESTDIR)$(includedir_internal)/libpq-int.h' '$(DESTDIR)$(includedir_internal)/pqexpbuffer.h' '$(DESTDIR)$(datadir)/pg_service.conf.sample'

clean distclean: clean-lib
	rm -f $(OBJS) pg_config_paths.h crypt.c getaddrinfo.c inet_aton.c noblock.c open.c pgstrcasecmp.c snprintf.c strerror.c strlcpy.c thread.c md5.c ip.c encnames.c wchar.c win32error.c pgsleep.c pthread.h exports.list
# Might be left over from a Win32 client-only build
	rm -f pg_config_paths.h

maintainer-clean: distclean
	rm -f $(srcdir)/libpqdll.def $(srcdir)/libpqddll.def $(srcdir)/blibpqdll.def $(srcdir)/libpq.rc
