#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/timezone/Makefile,v 1.28 2007/08/25 20:29:25 tgl Exp $
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

# use system timezone data?
ifneq (,$(with_system_tzdata))
override CPPFLAGS += '-DSYSTEMTZDIR="$(with_system_tzdata)"'
endif

all: SUBSYS.o

ifeq (,$(with_system_tzdata))
all: submake-libpgport zic
endif

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

zic: $(ZICOBJS)
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
ifeq (,$(with_system_tzdata))
	./zic -d '$(DESTDIR)$(datadir)/timezone' -p '$(POSIXRULES)' $(TZDATAFILES)
endif
	$(MAKE) -C tznames $@

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(datadir)'

uninstall:
ifeq (,$(with_system_tzdata))
	rm -rf '$(DESTDIR)$(datadir)/timezone'
endif
	$(MAKE) -C tznames $@

clean distclean maintainer-clean:
	rm -f SUBSYS.o zic$(X) $(OBJS) $(ZICOBJS)
