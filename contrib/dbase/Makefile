#
# $Header: /cvsroot/pgsql/contrib/dbase/Attic/Makefile,v 1.1 2001/05/10 14:41:23 momjian Exp $
#

subdir = contrib/dbase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= dbf.o dbf2pg.o endian.o

all: dbf2pg

dbf2pg: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -liconv -o $@

install: all installdirs
	$(INSTALL_PROGRAM) dbf2pg$(X)	$(bindir)
	$(INSTALL_DATA) README.dbf2pg	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/dbf2pg$(X) $(docdir)/contrib/README.dbf2pg

clean distclean maintainer-clean:
	rm -f dbf2pg$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
