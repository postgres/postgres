#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.6 2004/04/30 20:01:39 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -DPKGLIBDIR=\"$(pkglibdir)\"

OBJS= asctime.o difftime.o localtime.o pgtz.o
ZICOBJS= zic.o ialloc.o scheck.o localtime.o asctime.o pgtz.o

TZDATA := africa antarctica asia australasia europe northamerica southamerica pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES := $(TZDATA:%=data/%)

ifeq ($(USE_PGTZ), yes)
all: SUBSYS.o zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

zic: $(ZICOBJS)
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: zic
	./zic -d $(pkglibdir)/../timezone $(TZDATAFILES)

clean distclean maintainer-clean:
	rm -f SUBSYS.o $(OBJS) $(ZICOBJS)
endif

