#-------------------------------------------------------------------------
#
# GNUMakefile for psqlodbc (Postgres ODBC driver)
#
# $Header: /cvsroot/pgsql/src/interfaces/odbc/Attic/GNUmakefile,v 1.22 2001/10/09 22:32:33 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/odbc
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# Shared library parameters
ifeq ($(with_unixodbc),yes)
NAME = odbcpsql
else
NAME = psqlodbc
endif
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 27

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)


OBJS = info.o bind.o columninfo.o connection.o convert.o drvconn.o \
        environ.o execute.o lobj.o misc.o options.o \
        pgtypes.o psqlodbc.o qresult.o results.o socket.o parse.o statement.o \
        tuple.o tuplelist.o dlg_specific.o odbcapi.o

ifdef MULTIBYTE
OBJS += multibyte.o
endif

SHLIB_LINK += $(filter -lm -lnsl -lsocket, $(LIBS))
ifeq ($(with_unixodbc),yes)
SHLIB_LINK += -lodbcinst
endif
ifeq ($(with_iodbc),yes)
SHLIB_LINK += -liodbcinst
endif
ifeq ($(with_unixodbc)$(with_iodbc),nono)
OBJS += gpps.o
override CPPFLAGS += -DODBCINSTDIR='"$(odbcinst_ini_dir)"'
endif

all: all-lib odbc-drop.sql

# Shared library stuff
include $(top_srcdir)/src/Makefile.shlib

# Symbols must be resolved to the version in the shared library because
# the driver manager (e.g., iodbc) provides some symbols with the same
# names and we don't want those.  (This issue is probably ELF specific.)
LINK.shared += $(shlib_symbolic)

odbc-drop.sql: odbc.sql
	sed -n '/^CREATE OR REPLACE FUNCTION/s/CREATE OR REPLACE FUNCTION \([^ (][^ (]*([^)]*)\).*/DROP FUNCTION \1;/p' $< >$@

install: all installdirs
	$(INSTALL_DATA) $(srcdir)/odbc.sql $(DESTDIR)$(datadir)/odbc.sql
	$(INSTALL_DATA) odbc-drop.sql $(DESTDIR)$(datadir)/odbc-drop.sql
	$(MAKE) install-lib

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(datadir)

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(datadir)/odbc.sql $(DESTDIR)$(datadir)/odbc-drop.sql

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
	rm -f odbc-drop.sql
