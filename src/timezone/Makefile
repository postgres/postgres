#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.2 2004/04/30 04:31:52 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/tz
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS= asctime.o difftime.o localtime.o pgtz.o
ZICOBJS= zic.o ialloc.o scheck.o localtime.o asctime.o pgtz.o

TZDATA := africa antarctica asia australasia europe northamerica southamerica pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES := $(TZDATA:%=data/%)

ifeq ($(PGTZ), yes)
all: SUBSYS.o zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

zic: $(ZICOBJS)

install: zic
	zic -d $(datadir)/timezone $(TZDATAFILES)

clean distclean maintainer-clean:
	rm -f SUBSYS.o $(OBJS) $(ZICOBJS)
endif

