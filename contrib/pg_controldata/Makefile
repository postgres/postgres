#
# $Header: /cvsroot/pgsql/contrib/pg_controldata/Attic/Makefile,v 1.1 2001/02/23 20:38:35 momjian Exp $
#

subdir = contrib/pg_controldata
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= pg_controldata.o

all: pg_controldata

pg_controldata: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pg_controldata$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_controldata	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_controldata$(X) $(docdir)/contrib/README.pg_controldata

clean distclean maintainer-clean:
	rm -f pg_controldata$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
