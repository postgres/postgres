#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the access methods module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/Attic/Makefile.inc,v 1.2 1996/08/26 06:26:37 scrappy Exp $
#
#-------------------------------------------------------------------------

accdir=$(CURDIR)/access
VPATH:=$(VPATH):$(accdir):\
	$(accdir)/common:$(accdir)/gist:$(accdir)/hash:$(accdir)/heap:\
	$(accdir)/index:$(accdir)/rtree:$(accdir)/nbtree:$(accdir)/transam


SUBSRCS=
include $(accdir)/common/Makefile.inc
include $(accdir)/gist/Makefile.inc
include $(accdir)/hash/Makefile.inc
include $(accdir)/heap/Makefile.inc
include $(accdir)/index/Makefile.inc
include $(accdir)/rtree/Makefile.inc
include $(accdir)/nbtree/Makefile.inc
include $(accdir)/transam/Makefile.inc
SRCS_ACCESS:= $(SUBSRCS)

HEADERS+= attnum.h funcindex.h genam.h gist.h hash.h \
	heapam.h hio.h htup.h ibit.h iqual.h istrat.h \
	itup.h nbtree.h printtup.h relscan.h rtree.h \
	sdir.h skey.h strat.h transam.h tupdesc.h tupmacs.h \
	valid.h xact.h

