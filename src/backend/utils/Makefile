#
# Makefile for utils
#
# $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.18 2000/08/31 16:10:42 petere Exp $
#

subdir = src/backend/utils/
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS     := adt cache error fmgr hash init misc mmgr sort time
ifdef MULTIBYTE
SUBDIRS     += mb
endif
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)


all: SUBSYS.o fmgroids.h

SUBSYS.o: fmgrtab.o $(SUBDIROBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive)

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive): fmgroids.h
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

fmgroids.h fmgrtab.c: Gen_fmgrtab.sh $(top_srcdir)/src/include/catalog/pg_proc.h
	CPP='$(CPP)' AWK='$(AWK)' $(SHELL) $< $(top_srcdir)/src/include/catalog/pg_proc.h


clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o fmgrtab.o fmgroids.h fmgrtab.c

dep depend: fmgroids.h fmgrtab.c
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done

ifeq (depend,$(wildcard depend))
include depend
endif
