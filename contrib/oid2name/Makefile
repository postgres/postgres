# $Header: /cvsroot/pgsql/contrib/oid2name/Makefile,v 1.3 2001/02/20 19:20:27 petere Exp $

subdir = contrib/oid2name
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= oid2name.o

all: oid2name

oid2name: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@ -g

install: all installdirs
	$(INSTALL_PROGRAM) oid2name$(X) $(bindir)
	$(INSTALL_DATA) README.oid2name $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/oid2name$(X) $(docdir)/contrib/README.oid2name

clean distclean maintainer-clean:
	rm -f oid2name$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
