#
# $Header: /cvsroot/pgsql/contrib/findoidjoins/Attic/Makefile,v 1.12 2001/02/20 19:20:27 petere Exp $
#

subdir = contrib/findoidjoins
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

libpgeasy_srcdir = $(top_srcdir)/src/interfaces/libpgeasy
libpgeasy_builddir = $(top_builddir)/src/interfaces/libpgeasy

override CPPFLAGS := -I$(libpgeasy_srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= findoidjoins.o

all: findoidjoins

findoidjoins: $(OBJS) $(libpgeasy_builddir)/libpgeasy.a $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) -L$(libpgeasy_builddir) -lpgeasy $(libpq) $(LDFLAGS) $(LIBS) -o $@

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
