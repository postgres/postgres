#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/fmgr/Makefile,v 1.17 2004/05/21 20:56:49 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/fmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dfmgr.o fmgr.o funcapi.o

override CPPFLAGS += -DDLSUFFIX=\"$(DLSUFFIX)\"


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
