# src/test/modules/brin/Makefile

EXTRA_CLEAN = ./isolation_output
EXTRA_INSTALL=contrib/pageinspect

ISOLATIONCHECKS=summarization-and-inprogress-insertion

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/brin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

check: isolation-check

isolation-check: | submake-isolation
	$(MKDIR_P) isolation_output
	$(pg_isolation_regress_check) \
	    --outputdir=./isolation_output \
	    $(ISOLATIONCHECKS)

.PHONY: check isolation-check

submake-isolation:
	$(MAKE) -C $(top_builddir)/src/test/isolation all
