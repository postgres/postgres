# contrib/oid2name/Makefile

PGFILEDESC = "oid2name - examine the file structure"
PGAPPICON = win32

PROGRAM = oid2name
OBJS	= oid2name.o $(WIN32RES)

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/oid2name
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
