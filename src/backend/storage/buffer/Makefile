#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/buffer/Makefile,v 1.18 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/buffer
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = buf_table.o buf_init.o bufmgr.o freelist.o localbuf.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
