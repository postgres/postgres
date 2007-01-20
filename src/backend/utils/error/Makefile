#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/error/Makefile,v 1.12 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/error
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = assert.o elog.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
