#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/Makefile,v 1.5 1996/07/26 20:15:40 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../../mk
include $(MKDIR)/postgres.mk
include $(MKDIR)/postgres.user.mk

CFLAGS+=-I$(HEADERDIR)

#
# try locating libpq.a in the following places
#
LIBPQ:=  -L$(srcdir)/libpq/$(objdir) -L$(LIBDIR) -lpq

LDADD+= $(LIBPQ)


#
# build dynamically-loaded object files
#
DLOBJS= regress$(SLSUFF)

#
# ... plus test query inputs
#
CREATEFILES= $(DLOBJS:%=$(objdir)/%) \
	create.sql queries.sql errors.sql destroy.sql security.sql expected.out


OUTFILES= stud_emp.data onek.data regress.out aportal.out

CLEANFILES+= $(notdir $(CREATEFILES)) $(OUTFILES)

$(OUTFILES): $(CREATEFILES)
	$(SHELL) ./regress.sh 2>&1 | tee $(objdir)/regress.out
	@echo "RESULTS OF REGRESSION ARE SAVED IN $(objdir)/regress.out"

#
# expected results file -- expand macros for user and location
#
# the expected.input file is make by hand from 'regress.out' when the
# regression test has been updated and a successful output obtained
#
expected.out: expected.input
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $USER.'; exit 1; fi; \
	rm -f $(objdir)/expected.out; \
	C="`pwd`"; \
	sed -e "s:_CWD_:$$C:g" \
	   -e "s:_OBJWD_:$$C/$(objdir):g" \
	   -e "s/_USER_/$$USER/g" < expected.input > $(objdir)/expected.out

#
# prepare to run the test (including clean-up after the last run)
#
all:: $(CREATEFILES)
	cd $(objdir); rm -f $(OUTFILES)

#
# run the test
#
runtest: expected.out regress.out

#
# installation
#
install: localobj all

