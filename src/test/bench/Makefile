#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the Wisconsin Benchmark
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/bench/Attic/Makefile,v 1.1.1.1 1996/07/09 06:22:21 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../../mk
include $(MKDIR)/postgres.mk


CREATEFILES= create.sql bench.sql

include $(MKDIR)/postgres.user.mk


OUTFILES= bench.out bench.out.perquery
CLEANFILES+= $(CREATEFILES) $(OUTFILES)

bench.sql: 
	cat > $(objdir)/$@ < /dev/null
	x=1; \
	for i in `ls query[0-9][0-9]`; do \
		echo "select $$x as x" >> $(objdir)/$@; \
		cat $$i >> $(objdir)/$@; \
		x=`expr $$x + 1`; \
	done

bench2.pq: 
	cat > ${.TARGET} < /dev/null
	C=`pwd`; cd ${.CURDIR}; \
	for i in 1 2 3 4 5 6; do \
		echo "select timeofday();" >> $$C/${.TARGET}; \
	done; \
	x=1; \
	for i in `ls query[0-9][0-9]`; do \
		echo "select $$x as x;" >> $$C/${.TARGET}; \
		echo "select timeofday();" >> $$C/${.TARGET}; \
		cat $$i >> $$C/${.TARGET}; \
		echo "select timeofday();" >> $$C/${.TARGET}; \
		x=`expr $$x + 1`; \
	done

bench.out: $(CREATEFILES)
	$(SHELL) ./create.sh && \
	$(SHELL) ./runwisc.sh > $(objdir)/$@ 2>&1
	@echo "RESULTS OF BENCHMARK ARE SAVED IN ${MAKEOBJDIR}/bench.out";

bench.out.perquery: bench.out
	$(SHELL) ./perquery < $(objdir)/bench.out 2>&1 > $@
	@echo "BREAKDOWN OF BENCHMARK IS SAVED IN ${MAKEOBJDIR}/bench.out.perquery";

all:: $(CREATEFILES)
	rm -f $(OUTFILES)

runtest: ${OUTFILES}
