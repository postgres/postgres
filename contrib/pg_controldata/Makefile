#
# $Header: /cvsroot/pgsql/contrib/pg_controldata/Attic/Makefile,v 1.3 2001/03/13 01:17:40 tgl Exp $
#

subdir = contrib/pg_controldata
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS	= pg_controldata.o pg_crc.o

all: pg_controldata

pg_controldata: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

pg_crc.c: $(top_builddir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_controldata$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_controldata	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_controldata$(X) $(docdir)/contrib/README.pg_controldata

clean distclean maintainer-clean:
	rm -f pg_controldata$(X) $(OBJS) pg_crc.c

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
