#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for storage/buffer
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/buffer/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:53 scrappy Exp $
#
#-------------------------------------------------------------------------

SUBSRCS+= buf_table.c buf_init.c bufmgr.c freelist.c localbuf.c

SRCS_SITEMGR+= buf_table.c buf_init.c freelist.c
