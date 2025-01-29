# src/bin/pg_waldump/Makefile

PGFILEDESC = "pg_waldump - decode and display WAL"
PGAPPICON=win32

subdir = src/bin/pg_waldump
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(RMGRDESCOBJS) \
	$(WIN32RES) \
	compat.o \
	pg_waldump.o \
	rmgrdesc.o \
	xlogreader.o \
	xlogstats.o

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

ifeq ($(enable_percona_ext),yes)

OBJS += \
	$(top_srcdir)/src/fe_utils/simple_list.o \
	$(top_builddir)/src/libtde/libtde.a \
	$(top_builddir)/src/libtde/libtdexlog.a

override CPPFLAGS := -I$(top_srcdir)/contrib/pg_tde/src/include -I$(top_srcdir)/contrib/pg_tde/src/libkmip/libkmip/include $(CPPFLAGS)
endif

RMGRDESCSOURCES = $(sort $(notdir $(wildcard $(top_srcdir)/src/backend/access/rmgrdesc/*desc*.c)))
RMGRDESCOBJS = $(patsubst %.c,%.o,$(RMGRDESCSOURCES))


all: pg_waldump

pg_waldump: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

xlogstats.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

$(RMGRDESCSOURCES): % : $(top_srcdir)/src/backend/access/rmgrdesc/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_waldump$(X) '$(DESTDIR)$(bindir)/pg_waldump$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pg_waldump$(X)'

clean distclean:
	rm -f pg_waldump$(X) $(OBJS) $(RMGRDESCSOURCES) xlogreader.c xlogstats.c
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
