#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/fmgr/Makefile,v 1.12 2001/09/16 16:11:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/fmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dfmgr.o fmgr.o

override CPPFLAGS += -DPKGLIBDIR=\"$(pkglibdir)\" -DDLSUFFIX=\"$(DLSUFFIX)\"


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
