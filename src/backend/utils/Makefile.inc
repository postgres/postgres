#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the utilities modules
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Attic/Makefile.inc,v 1.2 1996/08/28 02:13:10 scrappy Exp $
#
#-------------------------------------------------------------------------

utilsdir= $(CURDIR)/utils
VPATH:= $(VPATH):$(utilsdir):\
	$(utilsdir)/adt:$(utilsdir)/cache:$(utilsdir)/error:$(utilsdir)/fmgr:\
	$(utilsdir)/hash:$(utilsdir)/init:$(utilsdir)/mmgr:$(utilsdir)/sort:\
	$(utilsdir)/time

SUBSRCS=
include $(utilsdir)/adt/Makefile.inc
include $(utilsdir)/cache/Makefile.inc
include $(utilsdir)/error/Makefile.inc
include $(utilsdir)/fmgr/Makefile.inc
include $(utilsdir)/hash/Makefile.inc
include $(utilsdir)/init/Makefile.inc
include $(utilsdir)/mmgr/Makefile.inc
include $(utilsdir)/sort/Makefile.inc
include $(utilsdir)/time/Makefile.inc
SRCS_UTILS:= $(SUBSRCS) fmgrtab.c

GENFMGRTAB= $(utilsdir)/Gen_fmgrtab.sh
#GENFMGRTABFILES= fmgr.h fmgrtab.c
GENFMGRTABFILES= fmgrtab.c

#
# BKIOPTS is set in ../catalog/Makefile.inc and sets the -D flags for
# the DATA(...); statements.  Hence, ../catalog/Makefile.inc had better
# get slurped in prior to this Makefile.inc, or BKIOPTS should be set
# in a higher directory level.
#
$(GENFMGRTABFILES): $(GENFMGRTAB) ../../include/catalog/pg_proc.h
	cd $(objdir); \
	sh $(SHOPTS) $(GENFMGRTAB) $(BKIOPTS) ../../include/catalog/pg_proc.h

$(objdir)/fmgrtab.o:  fmgrtab.c
	$(cc_inobjdir)

POSTGRES_DEPEND+= ${GENFMGRTABFILES}

#
#${PROG}: ${GENFMGRTABFILES}
#

CLEANFILES+= $(GENFMGRTABFILES)

HEADERS+= acl.h array.h bit.h builtins.h catcache.h datum.h \
	dynamic_loader.h elog.h exc.h excid.h fcache.h fmgrtab.h \
	geo-decls.h hsearch.h inval.h lselect.h lsyscache.h mcxt.h \
	memutils.h module.h nabstime.h oidcompos.h palloc.h  \
	portal.h psort.h rel.h rel2.h relcache.h sets.h \
	syscache.h tqual.h
