#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.79 2003/05/10 02:05:50 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 3
SO_MINOR_VERSION= 1

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS) -DFRONTEND -DSYSCONFDIR='"$(sysconfdir)"'

OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-print.o fe-lobj.o \
      pqexpbuffer.o pqsignal.o fe-secure.o \
      dllist.o md5.o ip.o wchar.o encnames.o \
      $(filter crypt.o getaddrinfo.o inet_aton.o snprintf.o strerror.o path.o, $(LIBOBJS))


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)
SHLIB_LINK += $(filter -lcrypt -ldes -lkrb -lcom_err -lcrypto -lk5crypto -lkrb5 -lssl -lsocket -lnsl -lresolv -lintl, $(LIBS))


all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend


# We use several backend modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.
# For port modules, this only happens if configure decides the module
# is needed (see filter hack in OBJS, above).

crypt.c getaddrinfo.c inet_aton.c snprintf.c strerror.c path.c: %.c : $(top_srcdir)/src/port/%.c
	rm -f $@ && $(LN_S) $< .

dllist.c: $(backend_src)/lib/dllist.c
	rm -f $@ && $(LN_S) $< .

md5.c: $(backend_src)/libpq/md5.c
	rm -f $@ && $(LN_S) $< .

ip.c: $(backend_src)/libpq/ip.c
	rm -f $@ && $(LN_S) $< .

encnames.c wchar.c : % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .


install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/libpq-int.h $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/pg_service.conf.sample $(DESTDIR)$(datadir)/pg_service.conf.sample

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) $(DESTDIR)$(includedir_internal)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(includedir)/libpq-fe.h $(DESTDIR)$(includedir_internal)/libpq-int.h $(includedir_internal)/pqexpbuffer.h

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) crypt.c getaddrinfo.c inet_aton.c snprintf.c strerror.c path.c
	rm -f $(OBJS) dllist.c md5.c ip.c wchar.c encnames.c
