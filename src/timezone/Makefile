#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the timezone library

# IDENTIFICATION
#    src/timezone/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "zic - time zone compiler"
PGAPPICON = win32

subdir = src/timezone
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# files to build into backend
OBJS= localtime.o strftime.o pgtz.o

# files needed to build zic utility program
ZICOBJS= zic.o $(WIN32RES)

# we now distribute the timezone data as a single file
TZDATAFILES = $(srcdir)/data/tzdata.zi

# which zone should determine the DST rules (not the specific UTC offset!)
# for POSIX-style timezone specs
POSIXRULES = US/Eastern

# any custom options you might want to pass to zic while installing data files
ZIC_OPTIONS =

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
# but GNU make versions <= 3.78.1 or perhaps later have a bug
# that causes a segfault;  GNU make 3.81 or later fixes this.
ifeq (,$(ZIC))
ZIC= ./zic
endif

zic: $(ZICOBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(ZICOBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
ifeq (,$(with_system_tzdata))
	$(ZIC) -d '$(DESTDIR)$(datadir)/timezone' -p '$(POSIXRULES)' $(ZIC_OPTIONS) $(TZDATAFILES)
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
