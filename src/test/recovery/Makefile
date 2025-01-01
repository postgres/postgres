#-------------------------------------------------------------------------
#
# Makefile for src/test/recovery
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/recovery/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL=contrib/pg_prewarm \
	contrib/pg_stat_statements \
	contrib/test_decoding \
	src/test/modules/injection_points

subdir = src/test/recovery
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export enable_injection_points

# required for 017_shm.pl and 027_stream_regress.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check
