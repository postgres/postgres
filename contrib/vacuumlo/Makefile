#
# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.10 2001/02/20 19:20:28 petere Exp $
#

subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= vacuumlo.o

all: vacuumlo

vacuumlo: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) vacuumlo$(X) $(bindir)
	$(INSTALL_DATA) README.vacuumlo $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/vacuumlo$(X) $(docdir)/contrib/README.vacuumlo

clean distclean maintainer-clean:
	rm -f vacuumlo$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
