#-------------------------------------------------------------------------
#
# GNUmakefile--
#    Makefile for regress (the regression tests)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/GNUmakefile,v 1.23 2000/08/31 16:12:26 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/regress
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

CONTRIB= $(top_builddir)/contrib

HOST := $(shell $(top_srcdir)/config/config.guess)

CFLAGS+= -I$(libpq_srcdir) $(CFLAGS_SL)

LDADD+= $(libpq)
        
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
# huge extra tests run in target bigtest
#
EXTRA_TESTS	= numeric_big

#
# prepare to run the tests
#
all: $(INFILES)
	cd input; $(MAKE) all; cd ..
	cd output; $(MAKE) all; cd ..
#ifneq ($(PORTNAME), win)
	$(MAKE) -C $(CONTRIB)/spi REFINT_VERBOSE=1 \
	refint$(DLSUFFIX) autoinc$(DLSUFFIX)
#else
#	cat /dev/null > $(CONTRIB)/spi/refint$(DLSUFFIX)
#	cat /dev/null > $(CONTRIB)/spi/autoinc$(DLSUFFIX)
#endif

#ifeq ($(PORTNAME), win)
#regress.dll: regress.c
#	cat /dev/null > $@
#endif

#
# run the tests
#
runtest: $(INFILES) 
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./regress.sh $(HOST) 2>&1 | tee regress.out
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./regress.sh $(HOST) 2>&1 | tee regress.out
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"
	@echo ""
	@echo "To run the optional big test(s) too, type 'make bigtest'"
	@echo "These big tests can take over an hour to complete"
	@echo "These actually are: $(EXTRA_TESTS)"

#
# run the test including the huge extra tests
#
bigtest: $(INFILES) 
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./regress.sh $(HOST) $(EXTRA_TESTS) 2>&1 | tee regress.out
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./regress.sh $(HOST) $(EXTRA_TESTS) 2>&1 | tee regress.out
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"

#
# run the parallel test suite
#
runcheck:	$(INFILES)
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./run_check.sh $(HOST)
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./run_check.sh $(HOST)
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILES run_check.out"
	@echo "AND regress.out"
	@echo ""
	@echo "To run the optional big test(s) too, type 'make bigcheck'"
	@echo "These big tests can take over an hour to complete"
	@echo "These actually are: $(EXTRA_TESTS)"

#
# run the test including the huge extra tests
#
bigcheck: $(INFILES) 
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./run_check.sh $(HOST) $(EXTRA_TESTS)
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./run_check.sh $(HOST) $(EXTRA_TESTS)
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILES run_check.out"
	@echo "AND regress.out"

clean:
	rm -f $(INFILES) regress.out run_check.out regress.o regression.diffs
ifeq ($(PORTNAME), win)
	rm -f regress.def
endif
	$(MAKE) -C sql clean
	$(MAKE) -C expected clean
	$(MAKE) -C results clean
	$(MAKE) -C $(CONTRIB)/spi clean
	rm -rf tmp_check
