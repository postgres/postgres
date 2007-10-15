# $PostgreSQL: pgsql/contrib/dict_xsyn/Makefile,v 1.1 2007/10/15 21:36:50 tgl Exp $

MODULE_big = dict_xsyn
OBJS = dict_xsyn.o
DATA_built = dict_xsyn.sql
DATA = uninstall_dict_xsyn.sql
DOCS = README.dict_xsyn
REGRESS = dict_xsyn

DICTDIR = tsearch_data
DICTFILES = xsyn_sample.rules

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/dict_xsyn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

install: install-data

.PHONY: install-data
install-data: $(DICTFILES)
	for i in $(DICTFILES); \
		do $(INSTALL_DATA) $(srcdir)/$$i '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i; \
	done

uninstall: uninstall-data

.PHONY: uninstall-data
uninstall-data:
	for i in $(DICTFILES); \
		do rm -rf '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i ; \
	done
