#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the Wisconsin Benchmark
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/test/bench/Makefile,v 1.10 2003/11/29 19:52:14 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/bench
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

CREATEFILES= create.sql bench.sql
OUTFILES= bench.out bench.out.perquery

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)


all: $(CREATEFILES)
	rm -f $(OUTFILES)

create.sql: create.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$C:g" \
	    -e "s:_SLSUFF_:$(SLSUFF):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

bench.sql: 
	x=1; \
	for i in `ls query[0-9][0-9]`; do \
	  echo "select $$x as x" >> bench.sql; \
	  cat $$i >> bench.sql; \
	  x=`expr $$x + 1`; \
	done

runtest: $(OUTFILES)

bench.out: $(CREATEFILES)
	$(SHELL) ./create.sh $$PGDATA && \
	$(SHELL) ./runwisc.sh $$PGDATA >bench.out 2>&1
	@echo "RESULTS OF BENCHMARK ARE SAVED IN FILE bench.out";

bench.out.perquery: bench.out
	$(SHELL) ./perquery <bench.out 2>&1 > $@
	@echo "BREAKDOWN OF BENCHMARK IS SAVED IN FILE" \
	  "bench.out.perquery";

clean:
	rm -f $(OUTFILES) $(CREATEFILES)
