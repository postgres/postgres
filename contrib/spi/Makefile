
SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/modules

CFLAGS+= $(CFLAGS_SL) -I$(SRCDIR)/include

ifdef REFINT_VERBOSE
CFLAGS+= -DREFINT_VERBOSE
endif

TARGETS= refint$(DLSUFFIX) refint.sql timetravel$(DLSUFFIX) timetravel.sql \
		autoinc$(DLSUFFIX) autoinc.sql moddatetime$(DLSUFFIX) moddatetime.sql \
		insert_username$(DLSUFFIX) insert_username.sql

CLEANFILES+= $(TARGETS)

all:: $(TARGETS)

install:: all $(CONTRIBDIR)
	$(INSTALL) -c README $(CONTRIBDIR)/README.spi
	for f in *.example *.sql *$(DLSUFFIX); do $(INSTALL) -c $$f $(CONTRIBDIR)/$$f; done

$(CONTRIBDIR):
	mkdir -p $(CONTRIBDIR)

%.sql: %.source
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$(CONTRIBDIR):g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean: 
	rm -f $(TARGETS) *.o
