#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/fmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/fmgr/Makefile,v 1.15 2003/12/23 21:56:20 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/fmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dfmgr.o fmgr.o funcapi.o

override CPPFLAGS += -DPKGLIBDIR=\"$(pkglibdir)\" -DDLSUFFIX=\"$(DLSUFFIX)\"


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)


# ensure that changes in PKGLIBDIR propagate to dfmgr.o
dfmgr.o: dfmgr.c $(top_builddir)/src/Makefile.global
