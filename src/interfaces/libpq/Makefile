#-------------------------------------------------------------------------
#
# Makefile for src/interfaces/libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq/Makefile,v 1.63 2002/07/18 03:59:49 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


# shared library parameters
NAME= pq
SO_MAJOR_VERSION= 2
SO_MINOR_VERSION= 2

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS) -DFRONTEND -DSYSCONFDIR='"$(sysconfdir)"'

OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-print.o fe-lobj.o \
      pqexpbuffer.o dllist.o md5.o pqsignal.o fe-secure.o \
      $(notdir $(INET_ATON)) $(notdir $(SNPRINTF)) $(notdir $(STRERROR))

ifdef MULTIBYTE
OBJS+= wchar.o encnames.o
endif


# Add libraries that libpq depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)
SHLIB_LINK += $(filter -lcrypt -ldes -lkrb -lcom_err -lcrypto -lk5crypto -lkrb5 -lssl -lsocket -lnsl -lresolv -lintl, $(LIBS))


all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib
backend_src = $(top_srcdir)/src/backend


dllist.c: $(backend_src)/lib/dllist.c
	rm -f $@ && $(LN_S) $< .

md5.c: $(backend_src)/libpq/md5.c
	rm -f $@ && $(LN_S) $< .

# We use several backend modules verbatim, but since we need to
# compile with appropriate options to build a shared lib, we can't
# necessarily use the same object files as the backend uses. Instead,
# symlink the source files in here and build our own object file.
# this only gets done if configure finds system doesn't have inet_aton()

ifdef INET_ATON
$(basename $(notdir $(INET_ATON))).c: $(basename $(INET_ATON)).c
	rm -f $@ && $(LN_S) $< .
endif

ifdef SNPRINTF
$(basename $(notdir $(SNPRINTF))).c: $(basename $(SNPRINTF)).c
	rm -f $@ && $(LN_S) $< .
endif

ifdef STRERROR
$(basename $(notdir $(STRERROR))).c: $(basename $(STRERROR)).c
	rm -f $@ && $(LN_S) $< .
endif

ifdef MULTIBYTE
wchar.c : % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .
encnames.c : % : $(backend_src)/utils/mb/%
	rm -f $@ && $(LN_S) $< .
endif


install: all installdirs install-lib
	$(INSTALL_DATA) $(srcdir)/libpq-fe.h $(DESTDIR)$(includedir)
	$(INSTALL_DATA) $(srcdir)/libpq-int.h $(DESTDIR)$(includedir_internal)
	$(INSTALL_DATA) $(srcdir)/pqexpbuffer.h $(DESTDIR)$(includedir_internal)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) $(DESTDIR)$(includedir_internal)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(includedir)/libpq-fe.h $(DESTDIR)$(includedir_internal)/libpq-int.h $(includedir_internal)/pqexpbuffer.h

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) dllist.c md5.c wchar.c encnames.c
	rm -f $(OBJS) inet_aton.c snprintf.c strerror.c
