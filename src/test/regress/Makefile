From scrappy@ki.net Thu Jul 25 05:51:19 1996
Status: RO
X-Status: 
Received: from daemun.ilt.com ([204.247.102.21]) by ki.net (8.7.5/8.7.5) with ESMTP id FAA12531 for <scrappy@ki.net>; Thu, 25 Jul 1996 05:51:16 -0400 (EDT)
From: postgres@madmax.ilt.com
Received: from madmax.ilt.com (madmax [199.26.203.43]) by daemun.ilt.com (8.7.5/8.7.3) with SMTP id CAA09205 for <scrappy@ki.net>; Thu, 25 Jul 1996 02:50:12 -0700 (PDT)
Received: by madmax.ilt.com (SMI-8.6/ILT-SVR4-1.1)
	id CAA00419; Thu, 25 Jul 1996 02:45:47 -0700
Date: Thu, 25 Jul 1996 02:45:47 -0700
Message-Id: <199607250945.CAA00419@madmax.ilt.com>
To: scrappy@ki.net
Subject: /user/postgres95/src/test/regress/Makefile == revised

#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regress (the regression test)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/Makefile,v 1.3 1996/07/25 19:56:58 scrappy Exp $
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

