#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for main
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/main/Makefile,v 1.10 2003/11/29 19:51:49 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/main
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = main.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
