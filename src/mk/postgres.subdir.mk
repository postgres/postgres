#-------------------------------------------------------------------------
#
# postgres.lib.mk--
#    include this to do recursive make on the subdirectories specified in
#    SUBDIR.
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/mk/Attic/postgres.subdir.mk,v 1.1.1.1 1996/07/09 06:22:19 scrappy Exp $
#
#-------------------------------------------------------------------------

.PHONY: all

.DEFAULT all:
	@for dir in $(SUBDIR); do \
		echo "===> $$dir"; \
		$(MAKE) -C $$dir --no-print-directory $@; \
	done
