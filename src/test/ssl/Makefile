#-------------------------------------------------------------------------
#
# Makefile for src/test/ssl
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/ssl/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL = contrib/sslinfo

subdir = src/test/ssl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export OPENSSL with_ssl

# The sslfiles targets are separated into their own file due to interactions
# with settings in Makefile.global.
.PHONY: sslfiles sslfiles-clean
sslfiles sslfiles-clean:
	$(MAKE) -f $(srcdir)/sslfiles.mk $@

clean distclean:
	rm -rf tmp_check
	$(MAKE) -f $(srcdir)/sslfiles.mk $@

# Doesn't depend on sslfiles because we don't rebuild them by default
check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
