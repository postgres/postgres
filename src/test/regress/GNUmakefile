#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/GNUmakefile,v 1.17 1999/01/17 06:19:57 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+= -I$(LIBPQDIR) -I../../include $(CFLAGS_SL)

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
	cd input; $(MAKE) all; cd ..
	cd output; $(MAKE) all; cd ..
#ifneq ($(PORTNAME), win)
	$(MAKE) -C ../../../contrib/spi REFINT_VERBOSE=1 refint$(DLSUFFIX) \
	autoinc$(DLSUFFIX)
#else
#	cat /dev/null > ../../../contrib/spi/refint$(DLSUFFIX)
#	cat /dev/null > ../../../contrib/spi/autoinc$(DLSUFFIX)
#endif

#ifeq ($(PORTNAME), win)
#regress.dll: regress.c
#	cat /dev/null > $@
#endif

#
# run the test
#
runtest: $(INFILES) 
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./regress.sh $(PORTNAME) 2>&1 | tee regress.out
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./regress.sh $(PORTNAME) 2>&1 | tee regress.out
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"

clean:
	rm -f $(INFILES) regress.out regress.o regression.diffs
ifeq ($(PORTNAME), win)
	rm -f regress.def
endif
	$(MAKE) -C sql clean
	$(MAKE) -C expected clean
	$(MAKE) -C results clean
	$(MAKE) -C ../../../contrib/spi clean
