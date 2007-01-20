#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/hash
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/hash/Makefile,v 1.12 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/hash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dynahash.o hashfn.o pg_crc.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
