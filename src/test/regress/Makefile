#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/Makefile,v 1.1.1.1 1996/07/09 06:22:24 scrappy Exp $
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
	create.sql queries.sql errors.sql destroy.sql security.sql


OUTFILES= stud_emp.data onek.data regress.out aportal.out

CLEANFILES+= $(notdir $(CREATEFILES)) $(OUTFILES)

$(OUTFILES): $(CREATEFILES)
	$(SHELL) ./regress.sh 2>&1 | tee $(objdir)/regress.out
	@echo "RESULTS OF REGRESSION ARE SAVED IN $(objdir)/regress.out"

#
# prepare to run the test (including clean-up after the last run)
#
all:: $(CREATEFILES)
	cd $(objdir); rm -f $(OUTFILES)

#
# run the test
#
runtest: regress.out

#
# installation
#
install: localobj all
