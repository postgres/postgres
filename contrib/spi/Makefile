
SRCDIR= /home/postgres/current/pgsql/src

include $(SRCDIR)/Makefile.global

CFLAGS+= $(CFLAGS_SL) -I$(SRCDIR)/include -DREFINT_QUIET

TARGETS= refint$(DLSUFFIX) refint.sql

CLEANFILES+= $(TARGETS)

all:: $(TARGETS)

%.sql: %.source
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$$C:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean: 
	rm -f $(TARGETS)
