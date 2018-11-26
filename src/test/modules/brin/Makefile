# src/test/modules/brin/Makefile

# Note: because we don't tell the Makefile there are any regression tests,
# we have to clean those result files explicitly
EXTRA_CLEAN = $(pg_regress_clean_files) ./isolation_output

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

check: isolation-check prove-check

isolation-check: | submake-isolation temp-install
	$(MKDIR_P) isolation_output
	$(pg_isolation_regress_check) \
	    --outputdir=./isolation_output \
	    $(ISOLATIONCHECKS)

prove-check: | temp-install
	$(prove_check)

.PHONY: check isolation-check prove-check

submake-isolation:
	$(MAKE) -C $(top_builddir)/src/test/isolation all
