#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tcop
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/tcop/Makefile,v 1.26 2006/10/08 17:15:34 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/tcop
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= dest.o fastpath.o postgres.o pquery.o utility.o

override CPPFLAGS += -DWIN32_STACK_RLIMIT=$(WIN32_STACK_RLIMIT)

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
