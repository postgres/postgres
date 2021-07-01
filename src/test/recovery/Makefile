#-------------------------------------------------------------------------
#
# Makefile for src/test/recovery
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/recovery/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/recovery
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# required for 017_shm.pl
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

check:
	$(prove_check)

clean distclean maintainer-clean:
	rm -rf tmp_check
