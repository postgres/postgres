#-------------------------------------------------------------------------
#
# Makefile for src/test/ssl
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/ssl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/ssl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_ssl

# The sslfiles targets are separated into their own file due to interactions
# with settings in Makefile.global.
.PHONY: sslfiles sslfiles-clean
sslfiles sslfiles-clean:
	$(MAKE) -f sslfiles.mk $@

clean distclean maintainer-clean:
	rm -rf tmp_check
	$(MAKE) -f sslfiles.mk $@

# Doesn't depend on sslfiles because we don't rebuild them by default
check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
