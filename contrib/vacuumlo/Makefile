# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.2 2000/05/29 05:44:32 tgl Exp $

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/contrib

CFLAGS+= -I$(LIBPQDIR)

TARGETS= vacuumlo
CLEANFILES+= $(TARGETS)
CURDIR=`pwd`

all:: $(TARGETS)

$(TARGETS): vacuumlo.o
	$(CC) -o vacuumlo -L $(LIBDIR) -lpq -lcrypt vacuumlo.o

clean: 
	rm -f $(TARGETS) *.o

dist:
	tar cf vacuumlo.tar README Makefile vacuumlo.c
