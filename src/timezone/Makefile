#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.24 2006/07/25 03:51:22 tgl Exp $
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
	./zic -d '$(DESTDIR)$(datadir)/timezone' $(TZDATAFILES)
	$(MAKE) -C tznames $@

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(datadir)'

uninstall:
	rm -rf '$(DESTDIR)$(datadir)/timezone'
	$(MAKE) -C tznames $@

clean distclean maintainer-clean:
	rm -f SUBSYS.o zic$(X) $(OBJS) $(ZICOBJS)
