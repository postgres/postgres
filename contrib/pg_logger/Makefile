#
# $Header: /cvsroot/pgsql/contrib/pg_logger/Attic/Makefile,v 1.1 2001/02/11 02:18:27 momjian Exp $
#

subdir = contrib/pg_logger
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS	= pg_logger.o

all: pg_logger

pg_logger: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pg_logger$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_logger	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_logger$(X) $(docdir)/contrib/README.pg_logger

clean distclean maintainer-clean:
	rm -f pg_logger$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
