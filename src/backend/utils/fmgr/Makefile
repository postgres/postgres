#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/fmgr/Makefile,v 1.11 2001/05/17 17:44:18 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/fmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dfmgr.o fmgr.o

override CPPFLAGS += -DLIBDIR=\"$(libdir)\" -DDLSUFFIX=\"$(DLSUFFIX)\"


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
