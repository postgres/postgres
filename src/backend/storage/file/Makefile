#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/file
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/file/Makefile,v 1.12 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/file
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = fd.o buffile.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
