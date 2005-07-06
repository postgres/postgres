#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.22 2005/07/06 21:40:09 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := $(CPPFLAGS)

# files to build into backend
OBJS= localtime.o strftime.o pgtz.o

# files needed to build zic utility program
ZICOBJS= zic.o ialloc.o scheck.o localtime.o

# timezone data files
TZDATA := africa antarctica asia australasia europe northamerica southamerica \
	pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES := $(TZDATA:%=$(srcdir)/data/%)

all: SUBSYS.o submake-libpgport zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

zic: $(ZICOBJS)
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	./zic -d $(DESTDIR)$(datadir)/timezone $(TZDATAFILES)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(datadir)

clean distclean maintainer-clean:
	rm -f SUBSYS.o zic$(X) $(OBJS) $(ZICOBJS)
