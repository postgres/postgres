#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    src/timezone/Makefile
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

include $(top_srcdir)/src/backend/common.mk

ifeq (,$(with_system_tzdata))
all: zic
endif

# We could do this test in the action section:
#	$(if $(ZIC),$(ZIC),./zic)
# but gmake versions <= 3.78.1 or perhaps later have a bug
# that causes a segfault;  gmake 3.81 or later fixes this.
ifeq (,$(ZIC))
ZIC= ./zic
endif

zic: $(ZICOBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
ifeq (,$(with_system_tzdata))
	$(ZIC) -d '$(DESTDIR)$(datadir)/timezone' -p '$(POSIXRULES)' $(TZDATAFILES)
endif
	$(MAKE) -C tznames $@

abbrevs.txt: zic $(TZDATAFILES)
	mkdir junkdir
	$(ZIC) -P -d junkdir -p '$(POSIXRULES)' $(TZDATAFILES) | LANG=C sort | uniq >abbrevs.txt
	rm -rf junkdir

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

uninstall:
ifeq (,$(with_system_tzdata))
	rm -rf '$(DESTDIR)$(datadir)/timezone'
endif
	$(MAKE) -C tznames $@

clean distclean maintainer-clean:
	rm -f zic$(X) $(ZICOBJS) abbrevs.txt
