#
# Makefile for optimizer
#
# $Header: /cvsroot/pgsql/src/backend/optimizer/Makefile,v 1.8 2000/07/13 16:06:44 petere Exp $
#

subdir = src/backend/optimizer
top_builddir = ../../..
include ../../Makefile.global

SUBDIRS     := geqo path plan prep util
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)

all: SUBSYS.o

SUBSYS.o: $(SUBDIROBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive)

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

clean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
	rm -f SUBSYS.o

dep depend: 
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
