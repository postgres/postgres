#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/Makefile,v 1.9 1996/11/13 10:36:17 bryanh Exp $
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
DLOBJS= regress$(SLSUFF)

#
# ... plus test query inputs
#
# INFILES is the files the regression test uses for input.
INFILES= $(DLOBJS) \
         create.sql queries.sql errors.sql destroy.sql security.sql \
         expected.out
#
# plus exports files
#
ifdef EXPSUFF
INFILES+= $(DLOBJS:.o=$(EXPSUFF))
endif

# OUTFILES is the files that get created by running the regression test.
OUTFILES= stud_emp.data onek.data regress.out aportal.out

#
# prepare to run the test (including clean-up after the last run)
#
all: $(INFILES)
	rm -f $(OUTFILES)

#
# run the test
#
runtest: $(INFILES) expected.out 
	$(SHELL) ./regress.sh 2>&1 | tee regress.out
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"

# The expected.input file is part of the distribution.  It was made by hand
# from 'regress.out' from a reference run of the regression test, replacing
# installation-dependent things with names like _CWD_.  The following rule
# turns those names back into real values for the instant installation to
# create a standard (expected.out) against which to compare regress.out 
# from the experimental run.

expected.out: expected.input
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $USER.'; exit 1; fi; \
	rm -f expected.out; \
	C="`pwd`"; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$C:g" \
            -e "s:_SLSUFF_:$(SLSUFF):g" \
	    -e "s/_USER_/$$USER/g" < expected.input > expected.out
	@echo "YOUR EXPECTED RESULTS ARE NOW IN FILE expected.out."

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$C:g" \
	    -e "s:_SLSUFF_:$(SLSUFF):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

clean:
	rm -f $(INFILES)
	rm -f $(OUTFILES)

