#
#	PostgreSQL lo type
#
#	Makefile pinched from the ip-mac contrib package
#
#	$Id: Makefile,v 1.1 1998/06/16 07:07:11 momjian Exp $

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/modules

CFLAGS+= $(CFLAGS_SL) -I$(SRCDIR)/include

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

TARGETS= lo$(DLSUFFIX) lo.sql

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
