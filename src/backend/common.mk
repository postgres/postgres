#
# Common make rules for backend
#
# $PostgreSQL: pgsql/src/backend/common.mk,v 1.1 2008/02/19 10:30:06 petere Exp $
#

SUBDIROBJS = $(SUBDIRS:%=%/SUBSYS.o)

all: SUBSYS.o

SUBSYS.o: $(SUBDIROBJS) $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

.PHONY: $(SUBDIRS:%=%-recursive)
$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

clean: clean-local
clean-local:
ifdef SUBDIRS
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit; done
endif
	rm -f SUBSYS.o $(OBJS)
