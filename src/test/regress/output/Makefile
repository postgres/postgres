#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/output/Attic/Makefile,v 1.1 1997/04/05 21:25:02 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../../..
include ../../../Makefile.global

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= create_misc.out \
         create_function.out

all: $(INFILES)

%.out: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $USER.'; exit 1; fi; \
	rm -f expected.out; \
	MYTZ=`date | cut -c21`; \
	C="`pwd`"; \
	sed -e "s:_CWD_:$(PWD):g" \
	    -e "s:_OBJWD_:$(PWD):g" \
            -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s;\([A-Z][a-z][a-z][^ ]* [A-Z][a-z][a-z] [0-9 ][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9] [0-9][0-9][0-9][0-9] \)[A-Z]\([A-Z][A-Z]\);\1$$MYTZ\2;g" \
	    -e "s;\([A-Z][a-z][a-z][^ ]* [A-Z][a-z][a-z] [0-9 ][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9] \)[A-Z]\([A-Z][A-Z] [0-9][0-9][0-9][0-9]\);\1$$MYTZ\2;g" \
	    -e "s:_USER_:$$USER:g" < $< > ../expected/$@
	@echo "YOUR EXPECTED RESULTS ARE NOW IN FILE expected.out."
