# contrib/pg_xlogdump/Makefile

PGFILEDESC = "pg_xlogdump"
PGAPPICON=win32

PROGRAM = pg_xlogdump
OBJS = pg_xlogdump.o compat.o xlogreader.o rmgrdesc.o \
	$(RMGRDESCOBJS) $(WIN32RES)

RMGRDESCSOURCES = $(notdir $(wildcard $(top_srcdir)/src/backend/access/rmgrdesc/*desc.c))
RMGRDESCOBJS = $(patsubst %.c,%.o,$(RMGRDESCSOURCES))

EXTRA_CLEAN = $(RMGRDESCSOURCES) xlogreader.c

ifdef USE_PGXS
$(error "pg_xlogdump cannot be built with PGXS")
endif

subdir = contrib/pg_xlogdump
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk


override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

xlogreader.c: % : $(top_srcdir)/src/backend/access/transam/%
	rm -f $@ && $(LN_S) $< .

$(RMGRDESCSOURCES): % : $(top_srcdir)/src/backend/access/rmgrdesc/%
	rm -f $@ && $(LN_S) $< .
