#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/output/Attic/Makefile,v 1.8 1997/08/28 04:49:23 vadim Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../../..
include ../../../Makefile.global

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= copy.out \
         create_function_1.out \
         create_function_2.out \
         misc.out \
	 constraints.out

all: $(INFILES)

%.out: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	OBJ=`pwd`; \
	sed -e "s:_OBJWD_:$$OBJ/\.\.:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< | \
	sed -e "s:output/\.\.:input/\.\.:g" > ../expected/$@

