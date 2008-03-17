#
# Makefile for utils
#
# $PostgreSQL: pgsql/src/backend/utils/Makefile,v 1.27 2008/03/17 19:44:41 petere Exp $
#

subdir = src/backend/utils
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS        = fmgrtab.o
SUBDIRS     = adt cache error fmgr hash init mb misc mmgr resowner sort time

include $(top_srcdir)/src/backend/common.mk

all: fmgroids.h probes.h

$(SUBDIRS:%=%-recursive): fmgroids.h

fmgroids.h fmgrtab.c: Gen_fmgrtab.sh $(top_srcdir)/src/include/catalog/pg_proc.h
	AWK='$(AWK)' $(SHELL) $< $(top_srcdir)/src/include/catalog/pg_proc.h

probes.h: probes.d
ifeq ($(enable_dtrace), yes)
	$(DTRACE) -h -s $< -o $@.tmp
	sed -e 's/POSTGRESQL_/TRACE_POSTGRESQL_/g' $@.tmp >$@
	rm $@.tmp
else
	sed -f $(srcdir)/Gen_dummy_probes.sed $< >$@
endif


clean:
	rm -f fmgroids.h fmgrtab.c probes.h
