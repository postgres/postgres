#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for bin/psql
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/bin/psql/Makefile,v 1.9 1996/11/08 06:01:27 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
LIBPQDIR = $(SRCDIR)/libpq
include ../../Makefile.global

INCLUDE_OPT := \
               -I$(LIBPQDIR) \
               -I$(SRCDIR)/include

CFLAGS += $(INCLUDE_OPT)
#
#USE_READLINE is set in Makefile.global
# 

ifeq ($(USE_READLINE), true)
   CFLAGS += $(READLINE_INC) $(HISTORY_INC)

   LIBCURSES=	-lcurses 
   LD_ADD += $(READLINE_LIB) $(HISTORY_LIB) $(LIBCURSES)

   ifeq ($(PORTNAME), ultrix4)
   LD_ADD += -ltermcap
   else
   ifeq ($(PORTNAME), sparc)
   LD_ADD += -ltermcap
   else
   ifeq ($(PORTNAME), linux)
   LD_ADD += -ltermcap
   else
   ifeq ($(PORTNAME), next)
   LD_ADD += -ltermcap
   else
   ifeq ($(PORTNAME), bsdi)
   LD_ADD += -ltermcap
   else
   ifeq ($(PORTNAME), BSD44_derived)
   LD_ADD += -ltermcap
   endif
   endif
   endif
   endif
   endif
   endif
else
   CFLAGS += -DNOREADLINE
endif

OBJS= psql.o stringutils.o 

ifneq ($(USE_READLINE), true)
OBJS+= rlstubs.o
endif

all: psql

psql: $(OBJS) $(LIBPQDIR)/obj/libpq.a
	$(CC) $(LDFLAGS) -o psql -L$(LIBPQDIR)/obj $(OBJS) -lpq $(LD_ADD)

$(srcdir)/libpq/obj/libpq.a:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install:
	$(INSTALL) $(INSTL_EXE_OPTS) psql $(DESTDIR)$(BINDIR)/psql

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f psql $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif



