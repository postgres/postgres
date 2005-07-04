#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.20 2005/07/04 19:54:51 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := $(CPPFLAGS) -I$(srcdir) -DNO_PGPORT

# files to build into backend
OBJS= localtime.o strftime.o pgtz.o

# files needed to build zic utility program
ZICOBJS= zic.o ialloc.o scheck.o localtime-zic.o

# timezone data files
TZDATA := africa antarctica asia australasia europe northamerica southamerica \
	pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES := $(TZDATA:%=$(srcdir)/data/%)

all: SUBSYS.o submake-libpgport zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

# In case of cross-compilation, zic needs to be built with a native
# compiler because it is run during the build, not on the final
# system.

localtime-zic.c: localtime.c
	$(LN_S) $< $@

$(ZICOBJS): CC=$(CC_FOR_BUILD)

zic: $(ZICOBJS)
	$(CC_FOR_BUILD) $(CFLAGS) $(ZICOBJS) -o $@$(X)

install: all installdirs
	./zic -d $(DESTDIR)$(datadir)/timezone $(TZDATAFILES)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(datadir)

clean distclean maintainer-clean:
	rm -f SUBSYS.o zic zic$(X) $(OBJS) $(ZICOBJS) localtime-zic.c
