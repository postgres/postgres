From scrappy@ki.net Fri Jul 26 05:15:00 1996
Status: RO
X-Status: 
Received: from daemun.ilt.com ([204.247.102.21]) by ki.net (8.7.5/8.7.5) with ESMTP id FAA02010 for <scrappy@ki.net>; Fri, 26 Jul 1996 05:14:58 -0400 (EDT)
Received: from darvish. ([199.26.203.37]) by daemun.ilt.com (8.7.5/8.7.3) with SMTP id CAA14746 for <scrappy@ki.net>; Fri, 26 Jul 1996 02:13:20 -0700 (PDT)
Received: by darvish. (5.x/SMI-SVR4)
	id AA07735; Thu, 25 Jul 1996 02:47:22 -0700
Date: Thu, 25 Jul 1996 02:47:22 -0700
From: postgres@darvish (POSTGRES)
Message-Id: <9607250947.AA07735@darvish.>
To: scrappy@ki.net
Subject: /user/postgres95/src/test/regress/Makefile == revision 2

#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/Makefile,v 1.4 1996/07/26 19:58:04 scrappy Exp $
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

