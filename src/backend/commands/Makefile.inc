#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the commands module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:18 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:=$(VPATH):$(CURDIR)/commands


SRCS_COMMANDS= async.c creatinh.c command.c copy.c defind.c define.c \
	purge.c remove.c rename.c vacuum.c version.c view.c cluster.c \
	recipe.c explain.c

HEADERS+= async.h command.h copy.h creatinh.h defrem.h purge.h \
	rename.h vacuum.h version.h view.h cluster.h \
	recipe.h


