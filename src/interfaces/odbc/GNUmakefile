#-------------------------------------------------------------------------
#
# GNUMakefile for psqlodbc (Postgres ODBC driver)
#
# $Header: /cvsroot/pgsql/src/interfaces/odbc/Attic/GNUmakefile,v 1.4 2000/09/18 20:11:37 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/odbc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
NAME = psqlodbc
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 26

CPPFLAGS += -I$(srcdir) -DHAVE_CONFIG_H -DODBCINSTDIR='"$(odbcinst_ini_dir)"'


OBJS = info.o bind.o columninfo.o connection.o convert.o drvconn.o \
        environ.o execute.o lobj.o misc.o options.o \
        pgtypes.o psqlodbc.o qresult.o results.o socket.o parse.o statement.o \
        gpps.o tuple.o tuplelist.o dlg_specific.o $(OBJX)

SHLIB_LINK= $(LD_FLAGS)

all: all-lib

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

LDFLAGS_SL+= $(LDFLAGS_ODBC)

odbc_headers = isql.h isqlext.h iodbc.h
odbc_includedir = $(includedir)/iodbc

install: all installdirs install-headers install-ini install-lib install-data

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(odbc_includedir) $(DESTDIR)$(libdir) $(DESTDIR)$(odbcinst_ini_dir) $(DESTDIR)$(datadir)

.PHONY: install-headers
install-headers: $(odbc_headers)
	for i in $^; do $(INSTALL_DATA) $$i $(DESTDIR)$(odbc_includedir)/$$i || exit 1; done

.PHONY: install-ini
install-ini: odbcinst.ini
	$(INSTALL_DATA) $< $(DESTDIR)$(odbcinst_ini_dir)/$<

.PHONY: install-data
install-data: odbc.sql
	$(INSTALL_DATA) $< $(DESTDIR)$(datadir)/$<

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
