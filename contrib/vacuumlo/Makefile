# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.1 1999/04/10 16:48:04 peter Exp $

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

CONTRIBDIR=$(LIBDIR)/contrib

CFLAGS+= -I$(HEADERDIR)

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
