#
#	PostgreSQL types for ISBN and ISSN identifiers.
#
#	$Id: Makefile,v 1.2 2000/05/29 05:44:26 tgl Exp $

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CFLAGS+= $(CFLAGS_SL)

all: isbn$(DLSUFFIX) issn$(DLSUFFIX)

install: isbn$(DLSUFFIX) issn$(DLSUFFIX)
	install -c isbn$(DLSUFFIX) issn$(DLSUFFIX) /usr/local/pgsql/modules

clean:
	rm -f isbn$(DLSUFFIX) issn$(DLSUFFIX) *.o
