#
# $Header: /cvsroot/pgsql/contrib/findoidjoins/Attic/Makefile,v 1.11 2000/12/04 01:32:18 tgl Exp $
#

subdir = contrib/findoidjoins
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(libpgeasy_srcdir) -I$(libpq_srcdir)

OBJS	= findoidjoins.o

all: findoidjoins

findoidjoins: $(OBJS) $(libpgeasy_builddir)/libpgeasy.a $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(libpgeasy) $(libpq) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) findoidjoins$(X)	$(bindir)
	$(INSTALL_SCRIPT) make_oidjoins_check	$(bindir)
	$(INSTALL_DATA) README.findoidjoins	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/findoidjoins$(X) $(bindir)/make_oidjoins_check $(docdir)/contrib/README.findoidjoins

clean distclean maintainer-clean:
	rm -f findoidjoins$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
