#
# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.9 2000/12/04 01:32:19 tgl Exp $
#

subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(libpq_srcdir)

OBJS	= vacuumlo.o

all: vacuumlo

vacuumlo: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(libpq) $(LIBS) -o $@

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
