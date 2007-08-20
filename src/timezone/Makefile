#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.27 2007/08/20 08:53:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# files to build into backend
OBJS= localtime.o strftime.o pgtz.o

# files needed to build zic utility program
ZICOBJS= zic.o ialloc.o scheck.o localtime.o

# timezone data files
TZDATA = africa antarctica asia australasia europe northamerica southamerica \
	pacificnew etcetera factory backward systemv solar87 solar88 solar89
TZDATAFILES = $(TZDATA:%=$(srcdir)/data/%)

# which zone should determine the DST rules (not the specific UTC offset!)
# for POSIX-style timezone specs
POSIXRULES = US/Eastern

all: SUBSYS.o submake-libpgport zic

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

ifeq (,$(with_system_tzdata))
zic: $(ZICOBJS)
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LIBS) -o $@$(X)
endif

install: all installdirs
ifeq (,$(with_system_tzdata))
	./zic -d '$(DESTDIR)$(datadir)/timezone' -p '$(POSIXRULES)' $(TZDATAFILES)
else
	ln -s '$(with_system_tzdata)' '$(DESTDIR)$(datadir)/timezone'
endif
	$(MAKE) -C tznames $@

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(datadir)'

uninstall:
	rm -rf '$(DESTDIR)$(datadir)/timezone'
	$(MAKE) -C tznames $@

clean distclean maintainer-clean:
	rm -f SUBSYS.o zic$(X) $(OBJS) $(ZICOBJS)
