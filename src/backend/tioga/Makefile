#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tioga
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tioga/Attic/Makefile,v 1.9 2000/10/20 21:03:50 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/tioga
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = tgRecipe.o Varray.o
override CPPFLAGS += -I$(srcdir)

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
