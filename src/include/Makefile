#-------------------------------------------------------------------------
#
# Makefile for src/include
#
# Install exported headers to the include directory (these headers are
# the minimal ones needed to build loadable backend extensions).
#
# $Header: /cvsroot/pgsql/src/include/Makefile,v 1.3 2000/08/31 16:11:27 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/include
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


HEADERS := os.h config.h c.h postgres.h postgres_ext.h fmgr.h \
	libpq/pqcomm.h libpq/libpq-fs.h lib/dllist.h \
	utils/geo_decls.h utils/elog.h utils/fmgroids.h utils/palloc.h \
	access/attnum.h executor/spi.h commands/trigger.h


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
	@for i in $(HEADERS); do \
	  echo "$(INSTALL_DATA) $(srcdir)/$$i $(includedir)/$$i"; \
	  $(INSTALL_DATA) $(srcdir)/$$i $(includedir)/$$i; \
	done

# Automatically pick out the needed subdirectories for the include
# tree.
installdirs:
	$(mkinstalldirs) $(addprefix $(includedir)/, $(sort $(dir $(HEADERS))))

uninstall:
	rm -f $(addprefix $(includedir)/, $(HEADERS))

clean:
	rm -f utils/fmgroids.h parser/parse.h

distclean maintainer-clean: clean
	rm -f config.h dynloader.h os.h stamp-h
