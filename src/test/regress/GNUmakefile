#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/GNUmakefile,v 1.7 1997/04/27 02:56:03 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+= -I$(LIBPQDIR) -I../../include

LDADD+= -L$(LIBPQDIR) -lpq
        
#
# DLOBJS is the dynamically-loaded object file.  The regression test uses 
# this when it does a CREATE FUNCTION ... LANGUAGE 'C').
#
DLOBJS= regress$(DLSUFFIX)

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= $(DLOBJS) 

#
# plus exports files
#
ifdef EXPSUFF
INFILES+= $(DLOBJS:.o=$(EXPSUFF))
endif

#
# prepare to run the test (including clean-up after the last run)
#
all: $(INFILES)
	cd input; gmake all; cd ..
	cd output; gmake all; cd ..

#
# run the test
#
runtest: $(INFILES) 
	$(SHELL) ./regress.sh 2>&1 | tee regress.out
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"

clean:
	rm -f $(INFILES)
	$(MAKE) -C sql clean
	$(MAKE) -C expected clean
	$(MAKE) -C results clean
