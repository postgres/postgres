#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tioga
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tioga/Attic/Makefile,v 1.8 2000/08/31 16:10:41 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/tioga
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = tgRecipe.o Varray.o
CPPFLAGS += -I$(srcdir)

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
