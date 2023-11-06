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
OBJS = \
	localtime.o \
	pgtz.o \
	strftime.o

# files needed to build zic utility program
ZICOBJS = \
	$(WIN32RES) \
	zic.o

# we now distribute the timezone data as a single file
TZDATAFILES = $(srcdir)/data/tzdata.zi

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
	$(ZIC) -d '$(DESTDIR)$(datadir)/timezone' $(ZIC_OPTIONS) $(TZDATAFILES)
endif
	$(MAKE) -C tznames $@

# Note: -P code currently depends on '-b fat'.  Not worth fixing right now.
abbrevs.txt: zic $(TZDATAFILES)
	mkdir junkdir
	$(ZIC) -P -b fat -d junkdir $(TZDATAFILES) | LANG=C sort | uniq >abbrevs.txt
	rm -rf junkdir

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)'

uninstall:
ifeq (,$(with_system_tzdata))
	rm -rf '$(DESTDIR)$(datadir)/timezone'
endif
	$(MAKE) -C tznames $@

clean distclean:
	rm -f zic$(X) $(ZICOBJS) abbrevs.txt
