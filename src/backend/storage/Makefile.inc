#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the storage modules
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:52 scrappy Exp $
#
#-------------------------------------------------------------------------

stordir= $(CURDIR)/storage
VPATH:= $(VPATH):$(stordir):$(stordir)/buffer:$(stordir)/file:$(stordir)/ipc:\
	$(stordir)/large_object:$(stordir)/lmgr:$(stordir)/page:$(stordir)/smgr

SUBSRCS=
include $(stordir)/buffer/Makefile.inc
include $(stordir)/file/Makefile.inc
include $(stordir)/ipc/Makefile.inc
include $(stordir)/large_object/Makefile.inc
include $(stordir)/lmgr/Makefile.inc
include $(stordir)/page/Makefile.inc
include $(stordir)/smgr/Makefile.inc
SRCS_STORAGE:= $(SUBSRCS)

HEADERS+= backendid.h block.h buf.h buf_internals.h bufmgr.h bufpage.h \
	fd.h ipc.h item.h itemid.h itempos.h \
	itemptr.h large_object.h lmgr.h lock.h multilev.h off.h page.h \
	pagenum.h pos.h proc.h shmem.h sinval.h sinvaladt.h smgr.h spin.h
