#
# Makefile for the access methods module
#
# $Header: /cvsroot/pgsql/src/backend/access/Makefile,v 1.8 2002/08/10 17:59:28 petere Exp $
#

subdir = src/backend/access
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS	    := common gist hash heap index nbtree rtree transam
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)

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

dep depend:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@ || exit; done
