#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# Install exported headers to the include directory (these headers are
# the minimal ones needed to build loadable backend extensions).
#
# $Header: /cvsroot/pgsql/src/include/Makefile,v 1.5 2000/10/20 21:04:03 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


srcdir_headers := c.h postgres.h postgres_ext.h fmgr.h \
	libpq/pqcomm.h libpq/libpq-fs.h lib/dllist.h \
	utils/geo_decls.h utils/elog.h utils/palloc.h \
	access/attnum.h executor/spi.h commands/trigger.h

builddir_headers := os.h config.h utils/fmgroids.h

HEADERS = $(srcdir_headers) $(builddir_headers)


all: $(HEADERS)


# These rules are necessary in case someone installs the include tree
# before building the backend tree. Very messy.
utils/fmgroids.h: $(top_builddir)/src/backend/utils/fmgroids.h
	cd utils && rm -f fmgroids.h && $(LN_S) ../$(top_builddir)/src/backend/utils/fmgroids.h .

# This file is built in the backend tree, but we need to keep track of
# the dependencies here.
$(top_builddir)/src/backend/utils/fmgroids.h: $(top_srcdir)/src/backend/utils/Gen_fmgrtab.sh catalog/pg_proc.h
	$(MAKE) -C $(dir $@) fmgroids.h


install: all installdirs
	for file in $(srcdir_headers); do \
	  $(INSTALL_DATA) $(srcdir)/$$file $(DESTDIR)$(includedir)/$$file || exit; \
	done
	for file in $(builddir_headers); do \
	  $(INSTALL_DATA) $$file $(DESTDIR)$(includedir)/$$file || exit; \
	done

# Automatically pick out the needed subdirectories for the include
# tree.
installdirs:
	$(mkinstalldirs) $(addprefix $(DESTDIR)$(includedir)/, $(sort $(dir $(HEADERS))))

uninstall:
	rm -f $(addprefix $(DESTDIR)$(includedir)/, $(HEADERS))

clean:
	rm -f utils/fmgroids.h parser/parse.h

distclean maintainer-clean: clean
	rm -f config.h dynloader.h os.h stamp-h
