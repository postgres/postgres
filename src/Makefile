#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Build and install postgres.
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/Makefile,v 1.6 1996/10/04 20:07:04 scrappy Exp $
#
# NOTES
#	objdir	- location of the objects and generated files (eg. obj)
#
#-------------------------------------------------------------------------

MKDIR=./mk
-include $(MKDIR)/../Makefile.global

FIND = find
# assuming gnu tar and split here
TAR  = tar
SPLIT = split

ETAGS = etags
XARGS = xargs

.DEFAULT all:
	$(MAKE) -C backend $@
	$(MAKE) -C libpq $@
ifeq ($(USE_TCL), true)
	$(MAKE) -C libpgtcl $@
endif
	$(MAKE) -C bin $@
	$(MAKE) -C ../doc $@
	@-chown -R $(POSTGRESLOGIN) $(POSTGRESDIR) 2>/dev/null
	@if test $@. = all. -o $@. = .; then \
	echo All of Postgres95 is successfully made.  Ready to install. ;\
        fi

TAGS:
	rm -f TAGS; \
	for i in backend libpq bin; do \
	  $(FIND) $$i -name '*.[chyl]' -print | $(XARGS) $(ETAGS) -a ; \
	done

# target to generate a backup tar file and split files that can be 
# saved to 1.44M floppy
BACKUP:
	rm -f BACKUP.filelist BACKUP.tgz; \
	$(FIND) . -not -path '*obj/*' -not -path '*data/*' -type f -print > BACKUP.filelist; \
	$(TAR) --files-from BACKUP.filelist -c -z -v -f BACKUP.tgz
	$(SPLIT) --bytes=1400k BACKUP.tgz pgBACKUP.	

.PHONY: TAGS
.PHONY: BACKUP
