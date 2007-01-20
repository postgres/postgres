#
# Makefile for the access methods module
#
# $PostgreSQL: pgsql/src/backend/access/Makefile,v 1.12 2007/01/20 17:16:10 petere Exp $
#

subdir = src/backend/access
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS	    := common gist hash heap index nbtree transam gin
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
