#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress/input (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/input/Attic/Makefile,v 1.9 2000/08/31 16:12:28 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/regress/input
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= copy.sql \
         create_function_1.sql \
         create_function_2.sql \
         misc.sql \
	 constraints.sql

all: $(INFILES)

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	OBJ=`pwd`; \
	sed -e "s:_OBJWD_:$$OBJ/\.\.:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s:_LIBDIR_:$(libdir):g" \
	    -e "s/_USER_/$$USER/g" < $< > ../sql/$@
