#-------------------------------------------------------------------------
#
# GNUMakefile for psqlodbc (Postgres ODBC driver)
#
# $Header: /cvsroot/pgsql/src/interfaces/odbc/Attic/GNUmakefile,v 1.6 2000/10/23 21:43:58 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/odbc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME = psqlodbc
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 26

override CPPFLAGS += -I$(srcdir) -DHAVE_CONFIG_H -DODBCINSTDIR='"$(odbcinst_ini_dir)"'


OBJS = info.o bind.o columninfo.o connection.o convert.o drvconn.o \
        environ.o execute.o lobj.o misc.o options.o \
        pgtypes.o psqlodbc.o qresult.o results.o socket.o parse.o statement.o \
        gpps.o tuple.o tuplelist.o dlg_specific.o $(OBJX)

SHLIB_LINK= -lm

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

odbc_headers = isql.h isqlext.h iodbc.h
odbc_includedir = $(includedir)/iodbc

install: all installdirs
	for i in $(odbc_headers); do $(INSTALL_DATA) $(srcdir)/$$i $(DESTDIR)$(odbc_includedir)/$$i || exit 1; done
	$(INSTALL_DATA) $(srcdir)/odbcinst.ini $(DESTDIR)$(odbcinst_ini_dir)/odbcinst.ini
	$(INSTALL_DATA) $(srcdir)/odbc.sql $(DESTDIR)$(datadir)/odbc.sql
	$(MAKE) install-lib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(odbc_includedir) $(DESTDIR)$(libdir) $(DESTDIR)$(odbcinst_ini_dir) $(DESTDIR)$(datadir)

uninstall: uninstall-lib
	rm -f $(addprefix $(DESTDIR)$(odbc_includedir)/, $(odbc_headers))
	rm -f $(DESTDIR)$(datadir)/odbc.sql
# XXX Uninstall the .ini file as well?

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
