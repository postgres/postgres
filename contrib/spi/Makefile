
SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CFLAGS+= $(CFLAGS_SL) -I$(SRCDIR)/include

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

TARGETS= refint$(DLSUFFIX) refint.sql timetravel$(DLSUFFIX) timetravel.sql

CLEANFILES+= $(TARGETS)

all:: $(TARGETS)

%.sql: %.source
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$$C:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean: 
	rm -f $(TARGETS)
