#
# Makefile for optimizer
#
# $PostgreSQL: pgsql/src/backend/optimizer/Makefile,v 1.13 2007/02/09 15:55:58 petere Exp $
#

subdir = src/backend/optimizer
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS     = geqo path plan prep util
SUBDIROBJS  = $(SUBDIRS:%=%/SUBSYS.o)

all: SUBSYS.o

SUBSYS.o: $(SUBDIROBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o
