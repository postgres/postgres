#
# $Header: /cvsroot/pgsql/contrib/pg_controldata/Attic/Makefile,v 1.2 2001/02/25 15:57:45 petere Exp $
#

subdir = contrib/pg_controldata
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= pg_controldata.o

all: pg_controldata

pg_controldata: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

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
