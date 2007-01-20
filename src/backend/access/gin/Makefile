#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gin
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/gin/Makefile,v 1.2 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = ginutil.o gininsert.o ginxlog.o ginentrypage.o gindatapage.o \
	ginbtree.o ginscan.o ginget.o ginvacuum.o ginarrayproc.o \
	ginbulk.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
