#
#	PostgreSQL types for IP and MAC addresses
#
#	$Id: Makefile,v 1.3 1998/04/22 04:14:11 scrappy Exp $

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/modules

CFLAGS+= $(CFLAGS_SL) -I$(SRCDIR)/include

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

TARGETS= ip$(DLSUFFIX) ip.sql mac$(DLSUFFIX) mac.sql

CLEANFILES+= $(TARGETS)

all:: $(TARGETS)

install:: all $(CONTRIBDIR)
	for f in *$(DLSUFFIX); do $(INSTALL) -c $$f $(CONTRIBDIR)/$$f; done

$(CONTRIBDIR):
	mkdir -p $(CONTRIBDIR)

%.sql: %.sql.in
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$(CONTRIBDIR):g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean: 
	rm -f $(TARGETS) *.o
