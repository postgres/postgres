#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.1 2004/04/30 04:09:23 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port/tz
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= asctime.o difftime.o localtime.o pgtz.o
ZICOBJS= zic.o ialloc.o scheck.o localtime.o asctime.o pgtz.o ../path.o

TZDATA := africa antarctica asia australasia europe northamerica southamerica pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES := $(TZDATA:%=data/%)

all: SUBSYS.o zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)


zic: $(ZICOBJS)

install: zic
	zic -d $(datadir)/timezone $(TZDATAFILES)

clean distclean maintainer-clean:
	rm -f SUBSYS.o $(OBJS) $(ZICOBJS)
