#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for bin/pg_id
#
# Copyright (C) 2000 by PostgreSQL Global Development Team
#
# $Header: /cvsroot/pgsql/src/bin/pg_id/Attic/Makefile,v 1.14 2000/01/20 21:51:07 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

OBJS= pg_id.o

all: pg_id

pg_id: $(OBJS)
	$(CC) -o pg_id $(OBJS) $(LDFLAGS)

install: pg_id
	$(INSTALL) $(INSTL_EXE_OPTS) pg_id$(X) $(BINDIR)/pg_id

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean:
	rm -f pg_id $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
