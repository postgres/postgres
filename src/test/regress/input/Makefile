#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/input/Attic/Makefile,v 1.3 1997/04/26 05:44:38 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../../..
include ../../../Makefile.global

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= copy.sql \
         create_function_1.sql \
         create_function_2.sql 

all: $(INFILES)

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	PWD=`pwd`; \
	OBJ=`pwd`; \
	sed -e "s:_CWD_:$(PWD):g" \
	    -e "s:_OBJWD_:$$OBJ/\.\.:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< > ../sql/$@
